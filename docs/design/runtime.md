# The runtime

Every Go++ program is linked with a runtime ([ADR-0035](../adr/0035-runtime-in-c23.md)): the start and the end of a program, the failure paths of the checks, and later allocation, buffers, type tables and the scheduler. Its lowest layer is GNU C23 in `runtime/`; everything that can be Go++ is in `std/`. This page says what is there today.

## Sources

| File | What it holds |
|---|---|
| `runtime/rt.c` | `_gopp_rt_init`, which the generated `main` calls first, `_gopp_rt_exit`, where every program that does not return from `main` ends, and `_gopp_rt_funcs`, which takes the names of the functions of a unit |
| `runtime/print.c` | `_gopp_print_<kind>`: the built-in `print` and `println` |
| `runtime/panic.c` | `_gopp_panic_<kind>`: where a failed bounds, slice, nil, division or shift check of generated code ends, and `_gopp_panic_must` |
| `runtime/string.c` | what the compiler calls for strings: `_gopp_string_concat`, `_gopp_string_append`, `_gopp_string_eq`, `_gopp_string_compare`, `_gopp_string_from_rune`, `_gopp_string_decode`, `_gopp_string_from_bytes`, `_gopp_string_from_runes`; and `_gopp_alloc`, which is `malloc` until the allocator of `P05.03` |
| `runtime/slice.c` | what the compiler calls for slices: `_gopp_slice_make`, `_gopp_slice_grow`, `_gopp_slice_append`, `_gopp_slice_copy`, `_gopp_slice_copy_into`, `_gopp_slice_clear`, `_gopp_slice_free`, `_gopp_slice_move_into`, `_gopp_bytes_from_string`, `_gopp_runes_from_string`, and `_gopp_each` |
| `runtime/map.c` | what the compiler calls for maps: `_gopp_map_make`, `_gopp_map_len`, `_gopp_map_read`, `_gopp_map_set`, `_gopp_map_delete`, `_gopp_map_clear`, `_gopp_map_copy`, `_gopp_map_drop`, and the three functions of a range loop, `_gopp_map_iter_start`, `_gopp_map_iter_next`, `_gopp_map_iter_end`; and what the equality and hash glue of a type calls: `_gopp_hash_bytes`, `_gopp_hash_string`, `_gopp_hash_float`, `_gopp_hash_each`, `_gopp_mem_eq`, `_gopp_each_eq` |
| `runtime/iface.c` | what the compiler calls for interface values: `_gopp_iface_box`, `_gopp_iface_clone`, `_gopp_iface_drop`, `_gopp_iface_eq`, `_gopp_iface_hash`, `_gopp_iface_table`, `_gopp_panic_assert` and `_gopp_panic_assert_iface` |
| `runtime/func.c` | what the compiler calls for function values: `_gopp_func_clone`, `_gopp_func_drop`, `_gopp_env_new`, and for the cells of captured variables `_gopp_cell_new`, `_gopp_cell_retain` and `_gopp_cell_release` |
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

## Strings

A string that a program makes while it runs (`a + b`, `string(r)`) has its bytes in a buffer of its own: a header `{refcount, byte_cap}`, the bytes, and one NUL (SLC-5); `backing` points to the header. A literal and the empty string have no buffer. The header counts the strings that share the bytes: `_gopp_string_retain` adds one for a copy of a string (and for a part `s[i:j]`, which shares the bytes), `_gopp_string_release` takes one away where a string ends, and the last one frees the buffer. The count is changed with atomic operations. Generated code asks first whether the string has a buffer (`backing != null`), so a literal calls neither.

- `_gopp_string_concat` makes a new buffer, except for a result without bytes.
- `_gopp_string_append(&s, more)` is `s += more` (STM-8). If the buffer of `s` has one count and room behind the bytes of `s`, the new bytes are written there and the NUL after them; no other string reads those bytes, since the count is one. Otherwise `s` gets a buffer with room for half as much again and 16 bytes more, and the old one is counted down last, because `more` may be a part of it. What `s` had is never changed, so a string that shares it is not touched.
- `_gopp_string_eq` compares the lengths, then the bytes; `_gopp_string_compare` gives a number below zero, zero or above it, which the compiler compares with zero for `<`, `<=`, `>`, `>=`.
- `_gopp_string_from_rune` is `string(r)`: the UTF-8 of the code point, or of U+FFFD for a number that is none.
- `_gopp_string_decode(text, at)` is one step of `range` over a string: the code point in the low half of the result and its width in the high half. Bytes that are no UTF-8 give U+FFFD and a width of 1, exactly where Go's `range` gives them.

## Slices

A slice with a buffer of its own has its elements after a header `{alloc_cap, allocator}` (SLC-1); `backing` points to the header and is null for a nil or empty slice and for a view. A slice of no elements that is not nil points at one byte of the runtime.

Every function takes a slice by the address of its four words, as a `void *`, because generated code has one struct for each element type; each gets the size of an element. The functions copy elements by their bytes. For elements whose copy is more than their bytes the compiler then calls `_gopp_each(first, count, size, each)`, which calls the clone glue of the element type for each of them ([gir.md](gir.md) §7).

- `_gopp_slice_make(slice, len, cap, size)` is `make`: zero elements, and Go's two messages for a length or a capacity out of range.
- `_gopp_slice_grow(slice, more, size, no_pointers)` makes room for `more` elements after the length. A slice with a buffer of its own and enough room keeps it; any other gets a new buffer, and its old one is freed, since the slice was the one owner. The new capacity is the one that Go's `growslice` gives: twice the old one below 256 elements, a quarter more above, rounded up to a size class of Go's allocator (the table of classes is in the file; `no_pointers` says which rounding Go uses).
- `_gopp_slice_append(slice, from, size, no_pointers)` is `append(s, t...)`; `from` is a slice or a string and may be the elements of the slice itself.
- `_gopp_slice_copy(slice, size)` makes the slice a copy of itself in a buffer of its own: the copy that a new owner gets.
- `_gopp_slice_copy_into(to, from, size)` is the built-in `copy`, and `_gopp_slice_clear` the built-in `clear`.
- `_gopp_bytes_from_string` and `_gopp_string_from_bytes` are the two conversions of `[]byte`; each copies the bytes. `_gopp_runes_from_string` and `_gopp_string_from_runes` are those of `[]rune`: bytes that are no UTF-8 give U+FFFD, one for each byte, and a number that is no code point is written as U+FFFD, as in Go. The slice of a conversion has room for its elements and no more; Go's has more, and how much depends on where the slice ends up.

`_gopp_slice_free(slice)` frees the buffer when the owner of the slice ends; the drop glue of the slice type calls it after it has ended the elements ([gir.md](gir.md) §7). `_gopp_slice_move_into(to, from, size, drop)` is `copy` for elements that hold what is freed: `from` is a slice of its own, whose elements are moved over those of `to`, which end first.

## Maps

A map is one pointer to a header `{allocator, count, iterators, seed, …}`; a nil map is a null pointer and is empty. The table has groups of eight slots. Each group has one word of eight control bytes, one for each slot: empty, deleted, or seven bits of the hash of the key that is there, so one comparison of words finds the slots that may hold a key. A key is looked for in the group that its hash names and then in the groups after it at growing steps, until a group with an empty slot. An eighth of the slots stays empty; a table that is full grows to twice its size, or is laid out anew at its size when half of it is slots of deleted keys.

Generated code has no type of the runtime for each map type. Every function takes a record of five numbers and six functions by its address (`gopp_mapinfo`: the size of a key, the size of a value, where the value is in a slot, the size of a slot, and how keys are hashed and compared), and keys and values by their addresses. Keys are compared by their bytes (integers, booleans, pointers, and structs and arrays of them without padding), as strings, or as floats, where the two zeros are one key and what is no number is never found again. Any other key type (a struct with a string, a float or padding in it, an array of strings) is hashed and compared by the hash and the equality glue of the type, which the record carries as its seventh and eighth words; for an interface value that glue is `_gopp_iface_hash` and `_gopp_iface_eq`. The sixth word is the clone glue of the values, or null for values that are copied by their bytes; `_gopp_map_copy` calls it for every value of the new table. The tenth and the eleventh words are the clone and the drop glue of the keys, for keys that hold what is freed, as a string does: a key that is put into the map is a copy of its own, which ends with its entry. Every map has a seed of its own for its hashes.

- `_gopp_map_make(shape, hint)` is `make` and a literal; a hint that is negative or too large is no hint, as in Go.
- `_gopp_map_read(map, shape, key, value)` writes the value of the key, or a zero, and says whether the key is there: `m[k]` and `v, ok := m[k]`.
- `_gopp_map_set(place, shape, key)` gives the address of the value of the key, which generated code then writes. A new key gets a zero value first. It takes the address of the place that holds the map, because a write to a nil map makes the map there (OWN-P9): no program ends with Go's `assignment to entry in nil map`.
- `_gopp_map_delete`, `_gopp_map_clear` and `_gopp_map_len` are the built-ins.
- `_gopp_map_copy(map, shape)` gives a map of its own with the same entries: the copy that a new owner gets.
- A range loop keeps three words in its frame. `_gopp_map_iter_start` picks a slot that no program can tell beforehand and counts the loop in the header; `_gopp_map_iter_next` copies the next key out and gives the address of its value in the map, or null after the last entry; `_gopp_map_iter_end` is called on every way out of the loop. While a loop is counted, a new key and `clear` end the program with `panic: map insert during iteration` or `panic: map clear during iteration` (RNG-4); writing a key that is there and `delete` are allowed.

`_gopp_map_drop(map, shape)` ends a map with its owner: its values, by the drop glue that the record carries as its ninth word, then its table and its header. `delete` and `clear` end the values that they remove the same way. A panic that leaves a loop does not end its count until cleanup blocks are lowered (`P04.15`).

## Interface values

An interface value is two words, `gopp_iface {itab, data}` (`TYP-5`). A nil interface value is two zero words.

- **The first word** says the type of the value that is held. For an interface without methods it is the address of the **descriptor** of the type; for an interface with methods it is the address of a table of methods, `_GPTa<type><interface>`, whose first word is that address and whose other words are what is called for each method of the interface, in the order of their names. The first word of a descriptor is its own address, so the runtime reads the type of both alike: `*(gopp_type **)itab`.
- **The second word** is the value itself when the descriptor says so: a value of at most one word that owns nothing (`TYP-8`), stored in the first bytes of the word with zeros after it. For any other type it is the address of a box that holds the value. The box has one owner, the interface value, and no count.

A descriptor is static data that generated code says (`gopp_type`, in `gopp_rt.h`): the size of a value, two marks (the value is in the word itself; two values are compared with `==`), four functions, the name of the type as the runtime of Go writes it (`interface {}`, `struct { x int }`, `main.Point`), and the methods of the type, or null for a type without methods. The functions are the clone, drop, equality and hash glue of the type ([gir.md](gir.md) §7); one that is null says that the bytes of a value are all there is to copy, end, compare or hash. A descriptor is a weak symbol, `_GPTi<type>`, that every package which needs it defines, so a program has one for a type, and two values are of one type exactly when their descriptors are one.

- `_gopp_iface_box(value, size)` gives a box with a copy of the bytes of a value.
- `_gopp_iface_clone(value)` makes the interface value at an address a copy of its own: a new box, and then the clone glue of the type for what the value holds. `_gopp_iface_drop(value)` calls the drop glue and frees the box. Both do nothing for nil and for a value that is in the word itself. The two are the clone and the drop glue of every interface type: a slice of interface values or a struct that holds one calls them as it calls any other glue.
- `_gopp_iface_eq(a, b)` is `==` on two interface values: both nil, or two values of one type that are equal. Two values of one type that is not compared end the program with `runtime error: comparing uncomparable type T`, as in Go. `_gopp_iface_hash(value, seed)` gives equal values one hash and ends the program with `runtime error: hash of unhashable type T` for a type that is not compared. The two are the equality and the hash glue of every interface type, so an interface value is a key of a map like any other value.
- `_gopp_iface_table(itab, wanted)` gives the first word that a value has as a value of another interface type: `itab` is the first word that it has now, `wanted` the record of the interface type (`gopp_itype`: its name and the keys of its methods). The result is null for nil and for a value whose type lacks a method; for an interface without methods it is the descriptor. The first time a type is asked for an interface type the table is made: each key of the interface is searched in the methods of the type (`gopp_methods`, in the order of the keys, which the descriptor points to), and the table is kept in a set by the two addresses, a missing method too. A process that runs one program after another forgets the tables when a program starts (`_gopp_rt_init`). Nothing locks the set yet: there is one thread until `P12`.
- `_gopp_panic_assert_iface(itab, wanted)` is where `x.(I)` ends for an interface type `I`: `panic: interface conversion: main.T is not main.I: missing method M`, or `interface is nil, not main.I`.
- `_gopp_panic_assert(itab, wanted, iface)` is where `x.(T)` ends for an `x` that holds no `T`: `panic: interface conversion: interface {} is string, not int`, with `nil` for an `x` that holds nothing.

## Function values

A function value is two words: the code, and the environment that the code is given as its first argument. The environment is null for a function that captured nothing. An environment (`gopp_env`, made by `_gopp_env_new(cells)`) has the count of the function values that share it, the number of its cells, and for each variable that the function captured the address of its cell and the drop glue of the variable, or null. `_gopp_func_clone(value)` counts one more value, and `_gopp_func_drop(value)` one less: the last value lets every cell of the environment go and frees the environment. A cell is a count and then the variable (`_gopp_cell_new(size)`, all zero but the count): the function that declares the variable and every environment that has it share it (`_gopp_cell_retain`), and the last that lets it go (`_gopp_cell_release(cell, drop)`) ends the variable by `drop` and frees the cell. The count is atomic, as that of a string. An environment in the frame of a function has the count `GOPP_ENV_IN_FRAME` and is never counted or freed. The two functions are the clone and the drop glue of every function type.

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
