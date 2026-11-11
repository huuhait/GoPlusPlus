# The C ABI layer

Go++ calls C with the exact C ABI of the target ([ADR-0001](../adr/0001-implementation-language.md), [ADR-0002](../adr/0002-clang-version-policy.md)). `crates/gopp-codegen-c` emits native GNU C23 `struct` definitions, `typedef`s, and function prototypes directly and compiles them with `clang` 23.1.0 (`--target=<triple>`), delegating register/stack classification, narrow-integer extension, homogeneous floating-point aggregates (HFAs), and indirect/struct-return conventions directly to Clang 23's target ABI implementation across every supported target (`amd64` SysV/Win64, `arm64` AAPCS64/Darwin/Win64, `riscv64` LP64D, `wasm32` WASI).

## Pieces

| Piece | Crate | Job |
|---|---|---|
| `CTypeRules` | `crates/gopp-gir` | Decides which Go++ types may cross the C ABI, shared by semantic checking (`E0442` invalid, `E0439` unsupported) and the GIR verifier (`V067`). Pure Rust, zero `libclang` dependency. |
| `CImport` | `crates/gopp-cimport` | Loads `libclang` 23.1.0 on demand (`clang-sys` `1.9.1` with `runtime` feature) for `@ImportC` header parsing (`P07`), and invokes `clang++ -std=c++20 -Xclang -ast-dump=json` plus layout probes when a package imports C++ headers (`P08`). |
| `C23Emitter` | `crates/gopp-codegen-c` | Emits C-compatible `struct` definitions with compile-time `_Static_assert(sizeof(...) == ... && _Alignof(...) == ...)` guards against `gopp-target`'s `TargetLayout`, plain C prototypes for `CallConv::C` declarations and `@Export("C")` thunks, and `__attribute__((swiftcall))` prototypes for `CallConv::Go` functions. |

## Which types cross (`gopp_gir::CTypeRules`)

- **Accepted:** `bool`, fixed-width and pointer-sized integers, `float32`/`float64`, pointers (`*T`, `Option[*T]`, `unsafe.Pointer`), structs of C types (nested, empty, or containing fixed-size arrays including length `0`), and `Unit` (`void`) as a return type.
- **Callbacks (`fn "C"`):** A function value passed to an `@Extern("C")` parameter has GIR type `fn "C"(...) -> ...` whose parameters and result are C types. Passing a top-level Go++ function wraps it in a deduplicated C-ABI thunk (`_GPTC...`); passing a C function passes the function pointer directly. Passing a closure, method value, or runtime function variable to C — or using a function value inside an `Option`/struct, returning one from C, or declaring one on `@Export("C")` — is rejected with `E0439` (`P07.08` / `P07.13`), because a Go++ `func` value is `{code, env}` (`16` bytes) whereas a C function pointer has no environment slot.
- **Refused with `E0442`:**
  - `Option[T]` by value for non-pointer `T` (`Option[*T]` is a nullable pointer and is accepted; non-pointer `Option[T]`'s `{T, flag}` layout is not a C ABI type until `P07` defines explicit FFI mappings);
  - `complex64` and `complex128` (until `P07` maps `_Complex`);
  - `string`, `[]T`, `map[K]V`, interfaces, `Unique[T]`, `Shared[T]`, `Weak[T]`, channels, and multi-result tuples;
  - a bare array `[N]T` as a top-level parameter or result (C decays array parameters to pointers; arrays inside structs are accepted);
  - `Unit` as a parameter;
  - an array of zero-size elements (`[N]struct{}` or `[N][0]T` for any `N`);
  - any type exceeding the complexity limits below.

### Complexity limits

Three hard bounds keep every recursive type walk fast in both `gopp-gir` and `clang`:

1. **Depth `64`:** maximum nesting levels of structs and arrays.
2. **`4096` expanded fields:** a struct counts itself plus each field; an array counts its element once (bounding DAGs that double at each level).
3. **`1 GiB`:** maximum byte size of any parameter or return type.

`CTypeRules` memoizes only intrinsic properties of each type (field verdict, layout, height, expanded field count — never the depth at which a query started), so verdicts never depend on query order. Walks cut off past `65` levels below the queried root are never memoized. When a type violates multiple rules, diagnostics report depth first, then the first non-C constituent, then expanded field count, then byte size.

## Layout agreement and C23 emission

1. **Compile-time layout verification:** For every aggregate that crosses a `CallConv::C` boundary (and every nested struct or array inside it), `gopp-codegen-c` emits `_Static_assert` checks on `sizeof`, `_Alignof`, and `offsetof` against `gopp-target`'s `TargetLayout`. Agreement holds by construction; any discrepancy halts C compilation immediately rather than silently corrupting a call.
2. **Aliasing-safe return slots:** Aggregate results returned from C calls are assigned into a fresh local temporary in emitted C23 before copying into an aliased destination (`x = c_fn(&x)`), preserving exact C evaluation and aliasing semantics.
3. **Direct calls, function pointers, and thunks:**
   - Direct `@Extern("C")` calls emit calls to standard C prototypes (`extern ret name(params...);`).
   - Indirect calls through C function pointers (`fn "C"`, including custom allocator vtable entries in [ADR-0029](../adr/0029-slice-and-map-allocator-word.md)) emit typed C function-pointer calls (`V076` restricts indirect C calls to scalar signatures until `P07.08`).
   - C-callable Go++ definitions (`main`, `@Export("C")` functions, and `_GPTC...` callback wrappers) are emitted as standard C functions whose bodies forward arguments to and return results from the underlying `__attribute__((swiftcall))` Go++ function ([calling-convention.md](calling-convention.md)).

## Verification

- **Unit & property tests (`crates/gopp-gir`, `crates/gopp-codegen-c`):** `CTypeRules` boundary tests (both sides of depth `64`, `4096` fields, `1 GiB`, zero-sized element arrays, query-order independence, and linear step counts on diamond type DAGs), verifier `V067`/`V075`/`V076` tests, and `insta` C23 snapshot tests for `@Extern("C")`, `@Export("C")`, and `_GPTC` thunks.
- **Differential round-trip tests (`tests/run/cabi-*`):** `proptest` / generator suites compile C callers and callees directly with `clang` 23.1.0 and link them against Go++ packages across scalar, nested-struct, HFA, `Option[*byte]`, 24-parameter register-exhaustion, `@Export("C")`, and callback signatures on `linux/amd64` and `linux/arm64` (native and `qemu-aarch64`), verifying byte-for-byte argument and return identity across `-opt=0`, `-opt=2`, and `-opt=3`.
