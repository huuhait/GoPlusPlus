# ADR-0036: Generated C includes the imported headers and calls C names directly

- **Status:** accepted (owner decision)
- **Date:** 2026-11-11
- **Plan reference:** D7.6, P07; related [ADR-0001](0001-implementation-language.md), [ADR-0008](0008-c-interop-open-questions.md), spec [chapter 14](../spec/14-foreign-surface.md)

## Context

A Go++ file imports a header (`import "<zlib.h>"`) and uses its names (`zlib.deflate(...)`). `gopp-cimport` reads the header with libclang to check those uses. What the generated C file then contains was not decided. Three ways exist:

1. include the header and call the C name;
2. never include it: emit our own prototype and struct definition for everything used;
3. put the header in a separate C file that wraps everything used, and call the wrappers.

Rust's bindgen and Zig re-declare (2) and need hand-written wrappers for macros and `static inline` functions. cgo uses wrappers (3) and pays a call. Languages that compile to C can include (1).

## Decision

1. **The generated file includes every header its package imports, in import order** ([FOR-5](../spec/14-foreign-surface.md)), and uses C functions, types, constants and macros by their C names. `gopp` never re-declares something it read from a header.
2. **Layout of a generated file that imports headers:**
   1. the includes;
   2. a `static inline` wrapper for each macro the package uses, so that later text does not depend on the macro;
   3. `#undef` of every macro whose name the generated code below writes as an identifier (a local, a parameter, a field);
   4. the generated code.
   The compiler knows every macro of the translation unit from libclang, so steps 2 and 3 are mechanical.
3. **Names the compiler invents at file scope start with `_GP` or `_gopp_`**, which no header may define.
4. **A local whose name hides a C name that the same function uses is renamed** in the generated code.
5. **A package that uses a header type through another package includes that header too.** Export data records the header and the flags it was read with.
6. C++ headers follow the same rule in a `.cpp` file (ADR-0001).
7. **Wrappers in a separate file (way 3) are the fallback for one header that cannot be included.** They are not built until a real library needs them.

## Consequences

- Macros (`errno`, `WIFEXITED`), `static inline` functions, bit-fields, packed and anonymous members, and symbol renames (`stat64`, the `$INODE64` names of macOS) work on every platform with no code of ours, and a C call costs nothing extra.
- One mechanism for C and C++.
- The header's names and macros share the file with generated code. Rules 2 to 4 are what keeps a header macro (`st_atime` in glibc, `min` and `interface` in the Windows headers) from rewriting generated code that carries Go++ source names for the debugger.
- A header that is not valid C23 does not compile, and two headers that cannot be included together cannot be imported by one package, as in C. A large header costs its parse time in every package that imports it.
- Headers must be present, with the same flags, when a dependent package is compiled.

## Alternatives

- **Re-declare (way 2):** isolates generated code completely and accepts headers of any C dialect, but macros and inline functions need a translator or wrappers, every layout and symbol rename must be reproduced by hand, and C++ cannot be done this way at all.
- **Wrappers for everything (way 3):** isolates too and handles every C feature, at the price of a call per use unless link-time optimization removes it, and accessors or copied layouts for struct fields.
