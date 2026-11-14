# Debug information

Debug information is produced jointly by `crates/gopp-gir` (`Function::Scopes`, [gir.md](gir.md) section 14) and `crates/gopp-codegen-c` compiling with **Clang 23 (`-gdwarf-5 -fno-omit-frame-pointer` at `-opt=0`)** ([ADR-0001](../adr/0001-implementation-language.md), [ADR-0002](../adr/0002-clang-version-policy.md)). Unless `-no-debug` is passed, `gopp-codegen-c` emits `#line <line> "<file>"` directives from GIR `Span`s, mirrors `DebugScope::Block` trees as nested C23 `{ ... }` lexical blocks, emits source-named C23 `struct`s and `Local::Source` variables, and binds mangled symbol names with `__asm__("_GP...")` so DWARF retains human-readable Go++ names while object symbols keep exact `_GP...` mangling.

`gopp build` writes debug information unless `-no-debug` is given; `gopp test-run` builds its programs without it unless `-debug` is given. A unit names the line of the source before every line of C that it writes, because C counts on from a line that it is told. It can name no column, so Clang is given `-gno-column-info`: a column would be one of the unit. The flags of Clang are those of an ELF target; on `windows/amd64` the unit has its `#line` directives and Clang is given no debug flag yet (section 5).

## 1. DWARF structure emitted via C23 + Clang 23

- **Compile unit (`DWARF 5`):** Compiled with `-gdwarf-5` (`DW_LANG_C11` / C23 in `gdb` 17 and `lldb`, both of which provide full struct/pointer/array expression evaluation).
- **Readable function names with exact `_GP...` linkage symbols:** `gdb` and `lldb` display `DW_AT_linkage_name` raw when it does not start with `_Z` (Itanium C++), so the linkage name is left out of the debug information (`-mllvm -dwarf-linkage-names=Abstract`) and a function is known by its name, which is the one that the source and a trace of a panic give it: `main.add`, `main.Vec.Len`, `main.Max[int]`, `main.main.func1`. A name of C has no `.` and no bracket, so the unit names the function with `$` in the place of each such byte (`main$add`, `main$Max$int$`; Clang takes `$` in a name, a name of Go++ has none), with the symbol as its `__asm__` label, and after Clang has made the file `gopp build` puts the real name in the place of that one in the strings of the debug information (`.debug_str`): the two have as many bytes, so nothing else moves (`gopp_codegen_c::debugger_names`, `named_for_debugger`). `break main.add` and a backtrace read as the source does; the symbols of the file stay those of Go++. A unit that is only written (`-emit=c`) has the names with `$`; a function that the compiler makes itself, a name that is not ASCII, and the second of two names that would be written alike keep `gopp_f<n>`. Compiler-generated functions (glue, thunks, wrappers) are written nowhere in the source: they carry `__attribute__((nodebug))`, so `gdb` and `lldb` never attribute their instructions to an adjacent user function's last line and step over them. What starts the program is attributed to the file name `gopp-generated`.
- **Debugger `finish` under `swiftcall` (`amd64` at `-opt=0`):**

| Result | Words | `swiftcall` registers (`amd64`) | `gdb` 17 `finish` | `lldb` `finish` |
|---|---|---|---|---|
| `int` | 1 | `rax` | exact | exact |
| `(int, int)` | 2 | `rax`, `rdx` | exact | exact |
| `(int, error)`, `(int, int, int)` | 3 | `rax`, `rdx`, `rcx` | SysV mismatch (needs `P14.07` debugger script) | zeros (needs `P14.07` script) |
| `(string, int)` | 4 | `rax`, `rdx`, `rcx`, `r8` | SysV mismatch | SysV mismatch |
| `(string, error)` | 5 | hidden output pointer | exact at `-opt=0` | exact at `-opt=0` |

- **Lexical scopes (`MaxBlockDepth = 64`):** Each `DebugScope::Block` containing source variables emits a nested C23 `{ ... }` compound block so shadowed variables (`x` inside an `if` hiding outer `x`) and loop variables (`i`) have exact DWARF `DW_TAG_lexical_block` lifetimes. Blocks deeper than `64` share the 64th lexical block (suppressing debug names only for variables that collide with an outer name in that shared block, keeping Clang DWARF emission fast on pathologically deep ASTs).
  - **How the unit is laid out (`emit/names.rs`, `Writer::scoped`):** the blocks of GIR are labels and jumps, so each statement is written in the block of C of its scope, and a block of GIR that goes from one scope to another is cut there, with a jump from one part to the next. The variables of a block are declared after its first label: Clang tells a debugger of no variable that is declared where nothing leads. The unit is compiled with `-Xclang -disable-lifetime-markers`: a block of C is a place for names, never the end of a value, as when every variable is declared at the top of the function.
  - **A variable keeps `_N`, and the program means the same,** when its name is one that C takes for something else (a word of C, a type name that the unit writes, `gopp…`, `T<digits>`, a name that is no ASCII identifier or starts with `_`); when a statement outside its scope names it; and when a statement of its scope names an outer variable of the same name (`x := x + 1`: the inner `x` has no name, so that `x` is never shown with the value of the other). The unit writes `#undef <name>` before a function for each name that it uses, so a macro of a header (`linux`, `errno`) is no obstacle.
  - **What a cleanup block ends is a field of the record of the function** (`gopp_own->text`): a variable that owns a string, a slice or another value that is freed is shown by `info locals` under `gopp_owned`, with its name as the name of the field, and is not found by its bare name. The debugger scripts (`P14.07`) are to show such a field as a variable.
- **Inlined sites (`DebugScope::Inlined`):** Trivial functions inlined by `gopp_gir::inline_trivial` emit `#line` directives pointing to the callee's file and line inside dedicated lexical blocks (`V073`, `V074`), or rely on `__attribute__((always_inline)) inline` C23 functions so Clang 23 emits native `DW_TAG_inlined_subroutine` frames with distinct call-site locations.
- **Variables (`Local::Source`) and `break f` prologue placement:**
  - Every `Local::Source` parameter, local variable, named result, and `range` binding is emitted with its Go++ source name; compiler temporaries (`Local::Temp`) are unnamed/artificial (`V011`).
  - Function entry setup (`Statement::Prologue`: zeroing named results, initializing closure capture pointers, storing `string` 3-word / `[]T` 4-word / `map[K]V` 1-word register arguments into local slots) is emitted before the first `#line` directive of the body, and `-opt=0` passes `-fno-omit-frame-pointer`. Consequently, both `break f` and `step` stop at the first body line with all parameters (`s`, `xs`, `m`, indirect `> 4`-scalar aggregates) already materialized in their stack slots.
  - `&` range aliases (`RNG-1a`, `Local::Deref`) and escaping closure cell captures expose the underlying value seamlessly.
- **Package globals:** Writable package variables emit with readable debug names (`main.counter`), while read-only compiler tables (`_GPTi...`, `_GPTa...`, `_GPTm...`) are marked artificial/unnamed in DWARF.

## 2. Type representation in C23 / DWARF

`gopp-codegen-c` maps every GIR type deterministically in one place (`emit_debug_type`):

| Go++ / GIR type | Emitted C23 / DWARF layout |
|---|---|
| `bool`, `int8`..`uint64`, `float32`, `float64` | C23 `bool`, `int8_t`..`uint64_t` (`uint8_t` = `unsigned char` so string `ptr` prints text), `float`, `double` |
| `string` (`24` bytes, [ADR-0026](../adr/0026-no-small-string-optimization.md), [ADR-0032](../adr/0032-shared-buffers-and-borrow-engine.md)) | `struct string { const uint8_t *ptr; uint64_t len; void *backing; }` |
| `[]T` (`32` bytes, [ADR-0029](../adr/0029-slice-and-map-allocator-word.md), [ADR-0041](../adr/0041-values-are-copied.md)) | `struct slice_<T> { T *ptr; uint64_t len; uint64_t cap; void *backing; }` (`backing` points to `{alloc_cap, alloc}`, or is null for a nil or empty slice and for a `&s[i:j]` view) |
| `[N]T` | Native C23 array `T[N]` |
| Named / generic struct (`Pair[int, string]`) | Named C23 `struct` with source field names (`Type::field_name`) and instantiated type arguments (`Type::type_args`) |
| `Option[T]` | Niche pointer/iface/func directly for pointer-like `T`; `struct { T value; bool set; }` otherwise |
| `map[K]V` | Pointer to runtime `struct gopp_map` (`count` = `len(m)`, `alloc`, ...; no reference count) |
| `interface` (`gopp_iface`) | `struct gopp_iface { void *itab; void *data; }` (`16` bytes) |
| `func` value (`gopp_func`) | `struct gopp_func { void *code; void *env; }` (`16` bytes); closures emit as `main_f_func1` with captured variables exposed by source name |
| Types with parents, `@Extends` (`P04.22`) | Embedded leading parent subobject fields at offset `0` (and tagged secondary bases) |

**What a unit writes today.** The values of a unit keep the type names of the unit (`T12`); for a debugger the struct of a declared type has a tag made of the name that the source gives it, with `_` where C takes no other letter (`struct main_Pair`, `struct main_Box_main_Pair` for `Box[Pair]`, `struct Unique_main_Pair`), so `ptype` shows the struct under that name. Its fields, and those of a struct without a name, have the names of the source; a field whose name C takes for something else, or that is written like the number of a field (`f3`), keeps its number. A tag is said once in a unit: a second type that would have the same keeps the name of the unit. A tag longer than 200 bytes is cut and ends in `_x` and the 64-bit `xxh3` of the symbol of the type. Slices, arrays, options and tuples keep the names of the unit, with the fields `ptr`, `len`, `cap`, `backing`, `a`, `value`, `set`; the two words of a function value are `code` and `env`.

### Deterministic type names, cycles, and height bounds

- **Canonical 200-byte name bound (`MaxNameLength = 200`):** Type names longer than 200 bytes are truncated and suffixed with `...#<hex>` where `<hex>` is the 64-bit `xxh3` (`xxhash-rust` `0.8.19`) compositional hash of the full type tree, preventing quadratic symbol/DWARF growth on deeply nested generics while keeping distinct types collision-free regardless of traversal order.
- **Cycles and deep pointer/array chains (`MaxTypeHeight = 48`):** Recursive struct and `Rec` types (`type S []S`, `type L [1]*L`, `type O Option[*O]`) emit forward `struct` declarations before definitions, and chains of pointers/arrays deeper than 47 levels wrap through named single-field wrapper structs so C23 declarations and DWARF type trees remain acyclic and linear in size.

## 3. Variables across `-opt` levels

- **At `-opt=0` (`clang -O0 -gdwarf-5 -fno-omit-frame-pointer`):** Every `Local::Source` variable is a genuine C23 stack variable in its lexical `{ ... }` block. Every store executes in program order and remains inspectable on every subsequent line of its scope.
- **At `-opt=1`..`-opt=z` (`clang -O1/-O2/-O3/-Os/-Oz -gdwarf-5`):** Clang promotes C23 stack variables to SSA registers and emits DWARF 5 location lists (`DW_AT_location`), reporting exact live register values or `<optimized out>`, never stale stack garbage.

## 4. Verification

- **Unit & stress tests (`crates/gopp-gir`, `crates/gopp-codegen-c`):** Verify `V073`/`V074` scope invariants, `.gopx` round-tripping of `Function::Scopes` and `field_name` tables, query-order independence of DWARF/C23 type emission, `MaxBlockDepth = 64` and `MaxTypeHeight = 48` caps, and linear step counts on 4000-type cycles and diamond ladders.
- **Scripted debugger & DWARF suite (`tests/debug/`):** Automated `gdb` 17 and `lldb` batch sessions (on `linux/amd64` and `windows/amd64`, [ADR-0037](../adr/0037-v1-targets.md)) testing `break f`, `step`, locals/globals of every Go++ type, shadowed scopes, closures, generics, interfaces, operator frames, and `<optimized out>` safety at `-opt=1..z`, plus DWARF structural verification across the full opt matrix.

## 5. Open points

- **Debug information on `windows/amd64`** is not designed. This document describes DWARF with `gdb` and `lldb`. The Windows target (`x86_64-pc-windows-msvc`) needs a choice between CodeView with a PDB file, which the Windows debuggers read, and DWARF, which only `lldb` reads there; the choice belongs to the work on the Windows target ([ADR-0037](../adr/0037-v1-targets.md)).
