# P07 — C interop

**Goal:** import any C header and call C with zero overhead, compile the C sources that live in a package, and ship `gopp cc`.
**Depends on:** P04, P06 · **Unblocks:** P08, P11, P13 · **Size:** L · **Milestone:** M4

## Decisions

**D7.1 — Embedded Clang, organized into import units.**
- A package's *import unit* is the list of headers it imports, plus their flags: target triple, language, standard, defines and include paths.
- **One Clang worker per package.** Each package gets its own Clang `CompilerInstance`, used on one thread. It answers the package's lookups in a fixed order, so the output is deterministic.
  - *Why:* Clang's Sema is not thread-safe, and the order of template instantiation changes the output.
- **Sharing.** Packages share only PCH files, never live ASTs. Two packages share a PCH only when their header lists and flags are equal.
- **Caching.** A unit is cached as a Clang PCH and as Go++ binding export data in the build cache. Export data also stores a header "recipe" for each foreign type it mentions, so a package that uses the type without importing its header can rebuild it (P06 D6.2).
- **Flags.** Header flags are set per package, not per file.
- **Order.** Header imports are processed in source order, with the package's files in name order. `gopp fmt` doesn't reorder them, and `import _ "<…>"` is allowed (P01 D1.15).
- **Conflicts.** Conflicting defines or C++ standards from different cdep targets in one package are an error.

**D7.2 — Lazy translation and identity.**
- **Lazy translation.** A declaration becomes a Go++ object the first time it is looked up (`zlib.crc32`), never eagerly. Huge headers (glibc, windows.h) cost only what is actually used.
- **Identity across units.**
  - A foreign entity's identity is its Clang USR (or its mangled name) plus a hash of its layout or signature.
  - It is a compile error when two packages import the same entity with a different layout, or with different flags that change inline bodies (`NDEBUG`, hardening modes).

**D7.3 — Surface.**
- A header import adds the header to the package's C import unit:
  - from a C/C++ package (cdep): the full Go-style path, e.g. `import "github.com/madler/zlib/zlib.h"`. It names exactly one file, parsed with the include dirs and defines of the CMake target that owns it (P11 D11.4);
  - from a Go++ module, yours or a dependency's: the same rule, e.g. `import "github.com/acme/app/internal/cdefs/defs.h"`;
  - system headers: angle brackets inside the string, `import "<stdio.h>"`, searched in the `-I…` dirs of the module's `cflags` (gopp.mod, P10), then the sysroot/system.
- **Classification and own-module headers** follow P01 D1.15:
  - the longest prefix over `require` and `cdep` paths decides whether an import is a package or a header;
  - a path inside a Go++ module that ends in a header extension (P01 D1.15) names a header: `.h` is C, the others are C++. Its flags come from the `cflags` and `cxxstd` directives in the gopp.mod of the module of the importing package (one Clang instance per package, P08/P10). A dependency's own `cxxstd` is a minimum: if the importer's is lower, the build fails with "module D needs c++23".
- **Each imported header is a package named after the file:** `zlib.h` → `zlib`, `<stdio.h>` → `stdio`, `<sys/socket.h>` → `socket`. Use it like any Go package: `zlib.crc32(…)`, `zlib.Z_OK`, `stdio.printf(…)`. There is no `C.` prefix.
  - An import can be renamed (`import z "github.com/madler/zlib/zlib.h"`). It must be renamed when the file name is a Go++ keyword or predeclared name, or isn't an identifier (`foo-bar.h`).
  - **Membership.** What a header package contains is decided by where each declaration belongs, not by the include graph:
    - C and C++ standard libraries: Clang's standard-library symbol → header table (the one clangd uses);
    - other system headers: mapping lists in the style of IWYU (glibc `bits/`, kernel `asm/` and `linux/`);
    - cdeps: the cdep's include tree. `zlib.h` includes `zconf.h` from the same tree, so `zlib.uLong` works;
    - other libraries' declarations come through their own imports;
    - the result must be the same with libc++ and libstdc++, and in C++20 and C++23.
  - A declaration seen through two header packages is one entity (D7.2), so values flow between them without conversion.
  - Names keep their C spelling and are always accessible, whatever their case. A name that is a Go++ keyword gets a trailing underscore (`pkg.type_`).
  - **Struct tags.** When a struct tag clashes with a function name, the tag is spelled `pkg.struct_name`, as in cgo: `stat.stat(…)` is the function and `stat.struct_stat` is the struct.
  - **Transparent unions.** A transparent-union parameter accepts any of the union's member types. Example: glibc's `bind` (with `_GNU_SOURCE`) takes a pointer to any `sockaddr` struct.

**D7.4 — Type mapping.**

| C | Go++ |
|---|---|
| `_Bool` | `bool` |
| `char` | `byte` (C's `char` signedness varies by target; strings are bytes) |
| `signed char` / `unsigned char` | `int8` / `uint8` |
| `short` / `unsigned short` | `int16` / `uint16` |
| `int` / `unsigned int` | `int32` / `uint32` (32-bit on every supported target) |
| `long` / `unsigned long` | `int64` / `uint64` on Linux and macOS, `int32` / `uint32` on Windows |
| `long long` / `unsigned long long` | `int64` / `uint64` |
| `wchar_t` | `int32` on Linux and macOS, `uint16` on Windows |
| `char8_t` / `char16_t` / `char32_t` | `uint8` / `uint16` / `uint32` |
| `size_t`, `ssize_t`, `intptr_t`, `uintptr_t` | follow the typedef, like any typedef: `uint64`, `int64`, `int64`, `uint64` on 64-bit Linux |
| `float` / `double` | `float32` / `float64` |
| `float _Complex` / `double _Complex` | `complex64` / `complex128` |
| `long double` | `ffi.LongDouble` (opaque; its size differs per target) |
| `__int128` / `unsigned __int128` | `ffi.Int128` / `ffi.Uint128` |
| `T*`, `const T*` | `Option[*T]`, because C may pass or return NULL; a pointer Clang knows is non-null (`_Nonnull`, `__attribute__((nonnull))`, `returns_nonnull`) is `*T`. A `*T` converts to `Option[*T]` automatically when passed to C (ADR-0018). `const` is recorded as metadata and enforced at the boundary: a read-only Go++ value can't be passed where C may change it (Q2, ADR-0008) |
| `void*` | `unsafe.Pointer` |
| `T[N]` | `[N]T` |
| struct | struct with Clang's exact layout (`ASTRecordLayout`), padding explicit |
| union | opaque struct of the right size and alignment, with typed accessor methods |
| bitfields | accessor methods |
| enum | named integer type + constants |
| typedef | alias in the header's package: `zlib.uLong` is `uint64` on Linux |
| function pointer | `Option[ffi.Func[func(…) R]]` (`ffi.Func[…]` when non-null, as for `T*`): no environment (Q1, ADR-0008); a C typedef name such as `c.cmp_fn` works too |
| flexible array member | accessor returning a `View[[]T]` |

There are no C-specific types such as `C.int`: every C type is a plain Go++ type, so values pass without conversions. Go's rule for numeric conversions still applies (`uint32(len(data))`). The few ABI-only types live in `ffi`, a small std package that most code never imports.

Several C types map to one Go++ type (on Linux, `long` and `long long` both become `int64`):
- For C++ overloads and templates, gopp keeps the exact C++ type. It uses a per-target reverse mapping: `int64` means the type of `int64_t` (`long` on Linux).
- The spellings `ffi.LongLong`, `ffi.Char`, `ffi.WChar` and `ffi.Char8`/`ffi.Char16`/`ffi.Char32` select the other types. Example: `vector.vector[ffi.LongLong]` is `std::vector<long long>`.

**D7.5 — C macros.**
- **Constants.** Object-like macros that evaluate to numeric constants become untyped Go++ constants. Their default type is the C type: `#define N 10u` defaults to `uint32`. String-literal macros become string constants.
- **Aliases.** A macro that names another identifier becomes an alias: with `#define crc32 z_crc32`, `zlib.crc32` is `zlib.z_crc32`.
- **Types.** A macro whose body is exactly one type becomes a type alias, like a typedef: `#define z_off_t long` makes `zlib.z_off_t` an alias of `int64` on 64-bit Linux (Q3, ADR-0008).
- **Function-like macros** are translated when the body is an expression over the parameters (the Zig translate-c approach). Parameter types are inferred from the body.
- **API notes.** Small per-library "API notes" files may add types that can't be inferred.
- Any other macro is not imported. The diagnostic suggests a `static inline` wrapper in a package-local header.

**D7.6 — Header inline code goes into the package's LLVM module.** Clang CodeGen emits `static inline` functions and other header bodies into the same LLVM module as the Go++ code, and only the ones the package references. Being in the same module, the optimizer can inline C helpers into Go++ loops.

**D7.7 — Calls, strings and memory: no conversions in human code.**
- Calls are direct and use the exact C ABI (D4.4). Variadic functions get C's default argument promotions.
- **Owned strings to C.** Owned strings are kept NUL-terminated (capacity len+1). So string literals and owned strings pass to a `const char*` parameter with no copy.
- **Views and borrowed parameters to C.** A `View[string]`, a substring or a borrowed `string` parameter (which may be a substring) passes as a temporary NUL-terminated copy that lives for the call (on the stack when short).
- **Inner NUL.** A string with an inner NUL panics when passed to C. For a constant string, it is a compile error.
- **Slices to C.** A `[]T` (owned, or borrowed from slicing) argument passes to a `T*`, `const T*` or `void*` parameter as its data pointer; a `View[[]T]` passes only to a `const T*` or `const void*` parameter (a view is read-only): `zlib.crc32(0, data, uint32(len(data)))`.
  - An empty slice passes a valid non-NULL pointer. Only a literal `nil` passes NULL.
  - *Why:* zlib's `crc32` returns its initial value when given NULL, so an empty slice must not become NULL.
- **Variadic arguments.** Strings pass as `const char*` (as above), slices pass as their data pointer, and other values get C's default promotions.
- **`std::string_view`.** A `View[string]` maps directly to a `std::string_view` parameter.
- **Vet.** `gopp vet` has a list of C functions that keep pointers after the call (`openlog`, `putenv`, `sqlite3_bind_text` with `SQLITE_STATIC`). It warns when one of them gets a Go++ string or slice.
- **errno** works as in cgo. In a two-value assignment, `n, err := unistd.read(fd, buf, uint64(len(buf)))` clears errno before the call and reads it right after, on the same thread, as an `error`.
  - `!` works on it: `n := unistd.read!(…)`.
  - Drop glue preserves errno.
- **What stays explicit.** Reading a string from C, and handing C a string it will keep, stay explicit, because a `char*` doesn't say how long it lives:
  - `ffi.String(p) View[string]` wraps a C string without copying; its lifetime is unchecked.
  - `ffi.CString(s)` returns an owned copy allocated with `malloc`, for C APIs that keep the pointer. It is freed on drop, or handed over to C with `.Release()`.

**D7.8 — Callbacks and panics across C.**
- Top-level Go++ functions, and function literals that capture nothing, pass directly where C expects a function pointer (type `ffi.Func[…]`) when their signatures are C-compatible.
- Capturing closures don't convert, because there are no trampolines. Use C's `void *userdata` convention instead.
- **Unwinding.** By default a panic never unwinds into C: the C-callback trampoline prints it, stops the other goroutines and exits with code 2, like any unrecovered panic (P04 D4.6).
  - A callback opts in to unwinding through its C callers with `@Unwind` (like Rust 1.81).
  - Unwinding needs unwind tables in the C frames, so gopp compiles all the C it builds with `-funwind-tables -fexceptions`.
- **Foreign threads.** A thread created by C (SDL audio, libuv) that calls Go++ is adopted into the runtime, like cgo's "extra M". If that isn't possible, the program aborts with a clear message. Built in P12.08.
- **Pinning.** A goroutine is pinned to its thread while C/C++ frames are on its stack, for example during a C → Go++ callback (P12 D12.9).
- **Under `-panic=abort`,** calls to C++ get terminate landing pads, so a C++ exception never unwinds through Go++ frames (P04 D4.6).

**D7.9 — Package-local C, C++ and assembly.**
- `.c`, `.h`, `.cc`/`.cpp`/`.cxx` and `.S` files in a package directory are compiled by the embedded Clang, one cached action per file.
- Flags come from the module's `cflags` and `ldflags` directives in gopp.mod (P01 D1.15, P10), and `//gopp:build` constraints apply.

**D7.10 — `gopp cc`, `gopp c++`, `gopp ar`.**
- These are drop-in compiler drivers for the selected target, with the same sysroot and libc as Go++ code (D7.11), in the `zig cc` model.
- CMake builds in P11 use them, which keeps the ABI consistent and simplifies cross-compilation.

**D7.11 — libc and cross-compilation.**
- Native builds use the host libc, found by host sysroot discovery (P00 D0.12).
- Linux cross builds use bundled musl, statically linked.
- `mode dynamic` cdeps and `csystem` libraries are unsupported on static-musl targets.
- Targeting a specific glibc version, a macOS SDK or a Windows SDK comes later, with documented requirements.

**D7.12 — Exporting to C.**
- `@Export("C")` on a function emits a C-ABI symbol.
- `gopp build -buildmode=c-archive|c-shared` produces a library plus a generated header, like Go's build modes.

## Testing strategy

- **ABI fuzzing (P07.12).**
  - A generator creates random C signatures: structs mixing scalar sizes, unions, arrays, floats, varargs.
  - Each signature is called in both directions (Go++ → C and C → Go++), and every value is compared.
  - It runs per target; cross targets run under QEMU user mode.
  - ABI fuzzing for C++ classes is in P08 (P08.14).
- **Header torture tests (P07.15):** all of libc's public headers, plus zlib, sqlite3, SDL2 and libcurl.

## Deliverables

- `compiler/lib/Import` in C mode, and header packages.
- Package-local C builds, and `gopp cc/c++/ar`.
- `@Export` with the c-archive/c-shared build modes.
- The ABI fuzz suite and the header torture suite.
- Samples:
  - zlib (system or cdep);
  - sqlite amalgamation (package-local);
  - qsort with a callback;
  - printf.

## Exit criteria

- M4: the samples run on linux/amd64 and linux/arm64.
- ABI fuzzing: at least 10 k signatures per target, with 0 mismatches.
- Importing `<stdio.h> <stdlib.h> <string.h> <math.h> <pthread.h>`, zlib and sqlite3 works, and warm (cached) import overhead is ≤ 20 ms per package (provisional).

## Risks

- **Header oddities** (GNU extensions, inline asm, `_Generic`) → Clang parses them. Translate what maps cleanly; skip the rest with a precise diagnostic. The header torture tests (P07.15) find them.
- **Clang is not thread-safe** → one Clang worker per package; packages share only PCH files (D7.1).
- **Sysroot availability for cross targets** → bundle musl; document the macOS and Windows SDK requirements.

## Open questions

Task P07.17 decides Q1–Q3.

- **Q1** (decided 2026-10-27, ADR-0008) C function pointer types are spelled `ffi.Func[func(…) R]`; no new syntax.
- **Q2** (decided 2026-10-27, ADR-0008) Constness stays metadata (no `*const T`); it is enforced at the C/C++ boundary against Go++'s read-only bindings, including `const` C++ methods and parameters (merged from P08 Q2).
- **Q3** (decided 2026-10-27, ADR-0008) Type-defining macros whose body is exactly one type are imported as type aliases (D7.5).
