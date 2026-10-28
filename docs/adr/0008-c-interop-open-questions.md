# ADR-0008: C interop open questions (function pointer types, constness, type macros)

- **Status:** accepted (changed by ADR-0020: the `ref` type is now `View[…]`, `@Ref` is `@View`, `ref` is no longer a contextual word, and `case T:` no longer matches a stored view)
- **Date:** 2026-10-27
- **Plan reference:** P07 Q1, Q2, Q3 (task P07.17); P07 D7.4, D7.5; P08 Q2 (merged into P07 Q2).

## Context

P07 left three questions open about how C and C++ declarations appear in Go++:

1. Code sometimes has to name a C function pointer type (a variable, a struct field, a parameter that is passed on to C). A Go++ `func` value is a closure (code pointer plus captured environment); a C function pointer is a code pointer only, so it needs its own type. Most code never names it: top-level functions and non-capturing literals pass directly, and a C `typedef` (`c.cmp_fn`) can be used as is.
2. C's `const T*` and C++ `const` methods and `const T&` parameters say "this is not changed". Go has no read-only pointers. Go++ already has read-only borrows (borrowed parameters, range variables, `ref` values).
3. Some C headers spell types as macros instead of typedefs (`#define z_off_t long` in zlib.h, `#define BOOL int`). D7.5 imported only constant, alias and expression macros.

The owner decided all three on 2026-10-27.

## Decision

1. **Function pointer types are spelled `ffi.Func[func(…) R]`.** It is an ordinary generic type in package `ffi`. No new syntax. Code that names it often can declare an alias (`type CmpFn = ffi.Func[func(a, b unsafe.Pointer) int32]`).
2. **Constness stays metadata.** `const T*` imports as `*T`; gopp records the `const` on parameters, pointees and C++ methods and enforces it at the boundary:
   - a read-only Go++ value (borrowed parameter, range variable, `ref` value) cannot be passed where C or C++ may change it (a non-`const` pointer or reference parameter);
   - only `const` C++ methods can be called on a borrowed or `ref` value (P08);
   - the string and slice passing rules of D7.7 use it.
   There is no `*const T` or other read-only pointer type in Go++.
3. **Type-defining macros are imported as type aliases** when the macro body is exactly one type (Clang parses it as a type name). `#define z_off_t long` makes `zlib.z_off_t` an alias of `int64` on 64-bit Linux, like a typedef. Any other macro shape still follows D7.5 (not imported, with the `static inline` / `typedef` hint).

## Consequences

- No new grammar: the parser, `gopp fmt` and the LSP need nothing for Q1 or Q2.
- The checker (P03 foreign scope, P07, P08) must carry a `const` flag on imported pointer parameters, pointees and methods, and check it against Go++'s read-only bindings.
- Go++ code cannot express "read-only pointer" in its own types; a pure Go++ API that wants it uses a borrowed parameter or `ref` instead.
- The macro importer (P07, D7.5) gains one more case: type macros become aliases.

## Alternatives

- **Q1: a dedicated form** such as `extern "C" func(…)` (Rust) or `@convention(c)` (Swift). Rejected: new grammar in type position, not Go-like, and `macro` is meant to stay the only new reserved word.
- **Q2: a read-only pointer type** (`*const T`, as in Rust, Zig and Swift). Rejected: every pointer gets two kinds in the whole language, even in code that never touches C; the existing read-only borrows already cover the boundary checks.
- **Q3: not importing type macros** (cgo, Swift). Rejected: common libraries such as zlib use them, Clang already tells whether a macro is a type, and users would otherwise write wrapper headers.
