# The C ABI layer

Go++ calls C with the exact C ABI of the target ([ADR-0001](../adr/0001-implementation-language.md), [ADR-0002](../adr/0002-clang-version-policy.md)). `crates/gopp-codegen-c` emits native GNU C23 `struct` definitions, `typedef`s, and function prototypes directly and compiles them with `clang` 23.1.0 (`--target=<triple>`), delegating register/stack classification, narrow-integer extension, homogeneous floating-point aggregates (HFAs), and indirect/struct-return conventions directly to Clang 23's target ABI implementation across every target of the `TargetSpec` table (`amd64` SysV/Win64, `arm64` AAPCS64/Darwin/Win64, `riscv64` LP64D, `wasm32` WASI).

## Pieces

| Piece | Crate | Job |
|---|---|---|
| `CTypeRules` | `crates/gopp-gir` | Decides which types may cross the C ABI, shared by the lowering, which reports what a declaration gets wrong (`E0442` for `@Extern`, `E0445` for `@Export`, `E0439` for what is not supported yet), and the GIR verifier (`V067`). Pure Rust, zero `libclang` dependency. |
| `CImport` | `crates/gopp-cimport` | Loads `libclang` 23.1.0 on demand (`clang-sys` `1.9.1` with `runtime` feature) to read imported headers (`P07`), and invokes `clang++ -std=c++20 -Xclang -ast-dump=json` plus layout probes when a package imports C++ headers (`P08`). |
| `C23Emitter` | `crates/gopp-codegen-c` | Emits C-compatible `struct` definitions with compile-time `_Static_assert(sizeof(...) == ... && _Alignof(...) == ...)` guards against `gopp-target`'s `TargetLayout`, plain C prototypes for `CallConv::C` declarations and `@Export("C")` thunks, and `__attribute__((swiftcall))` prototypes for `CallConv::Go` functions. |

## Which types cross (`gopp_gir::CTypeRules`)

- **Accepted:** `bool`, fixed-width and pointer-sized integers, `float32`/`float64`, pointers (`*T`, `Option[*T]`, `unsafe.Pointer`), structs of C types (nested, or containing fixed-size arrays including length `0`), and nothing at all (`void`) as a result.
- **Callbacks (`fn "C"`):** A parameter of function type of an `@Extern("C")` function has the GIR type `fn "C"(...) -> ...`, whose parameters and result are C types. Passing a top-level Go++ function gives C the function of C that the package makes for it once (`_GPTC...`); passing an `@Extern("C")` function passes that function itself. Passing a closure, a method value or a function variable to C, a function type inside an `Option` or a struct, as a result, as a parameter of a callback, or as a parameter of an `@Export("C")` function is rejected with `E0439` (`P07.08` / `P07.13`), because a Go++ `func` value is `{code, env}` (`16` bytes) whereas a C function pointer has no environment slot.
- **Refused with `E0442` (`E0445` on an `@Export("C")` function):**
  - `Option[T]` by value for non-pointer `T` (`Option[*T]` is a nullable pointer and is accepted; non-pointer `Option[T]`'s `{T, flag}` layout is not a C ABI type until `P07` defines explicit FFI mappings);
  - `complex64` and `complex128` (until `P07` maps `_Complex`);
  - `string`, `[]T` (so a `...T` parameter too), `map[K]V`, interfaces, `Unique[T]`, `Shared[T]`, `Weak[T]`, channels, and several results;
  - a bare array `[N]T` as a top-level parameter or result (C decays array parameters to pointers; arrays inside structs are accepted);
  - a value without a size, by itself or inside a struct: a struct without fields (`struct{}`, or one whose fields all take no room) and an array of such elements (`[N]struct{}` or `[N][0]T` for any `N`). C has no one layout for it: Clang 23 gives `struct {}` no room on `linux/amd64` and four bytes on `windows/amd64` (measured), so the same declaration would mean two layouts. An array of length `0` of elements that have a size is accepted as a field: it takes no room on both, and what follows it starts where its elements may;
  - any type exceeding the complexity limits below.
- **The name** of an `@Extern("C")` or `@Export("C")` function is its C symbol. A name that starts with `_GP` or `_gopp_` is refused (`E0442`, `E0445`): those are the names that Go++ and its runtime make ([appendix A.8](../spec/appendix-a-symbol-names.md)). `@Export("C")` on `main` is refused too.

A handle or a map is a pointer in GIR and a function value two words, so the lowering looks at the checked type of each parameter first; `CTypeRules` then decides on the GIR type, which is all the verifier has.

### Complexity limits

Three hard bounds keep every type walk fast in both `gopp-gir` and `clang`:

1. **Depth `64`:** maximum nesting levels of structs and arrays.
2. **`4096` fields:** a scalar counts one, a struct itself plus its fields, an array itself plus its element once, however long it is (bounding DAGs that double at each level).
3. **`1 GiB`:** maximum byte size of any parameter or return type.

`CTypeRules` keeps for each type only what the type is by itself (its height, its count of fields, its first part that is no C type, whether it takes room), never where a question started, so a verdict never depends on what was asked before. The walk keeps its work in a list and works each type out once: the steps grow with the types, in every order of asking. When a type breaks several rules, the diagnostic names the depth first, then the first part that is no C type, then the count of fields, then the byte size. The verifier has no target, so `V067` does not count the bytes.

## Layout agreement and C23 emission

A package that imports headers includes them in its generated file and uses their declarations as they are ([ADR-0036](../adr/0036-generated-c-includes-headers.md)). What follows is about the declarations `gopp` writes itself: Go++ types, `@Extern("C")` functions without a header, and `@Export("C")` thunks.

1. **Compile-time layout verification:** Every struct of a unit has a `_Static_assert` on `sizeof` and `_Alignof` against `gopp-target`'s `TargetLayout`; for every aggregate that crosses a `CallConv::C` boundary (and every nested struct inside it), `gopp-codegen-c` also checks the `offsetof` of each field. Agreement holds by construction; any discrepancy halts C compilation immediately rather than silently corrupting a call.
2. **Aliasing-safe return slots:** The result of a call that has several parts is put into a temporary of its own, which nothing points into (`V066`), before it is copied to where the program wants it (`x = c_fn(&x)`), preserving exact C evaluation and aliasing semantics.
3. **Direct calls, function pointers, and thunks:**
   - Direct `@Extern("C")` calls emit calls to standard C prototypes (`extern ret name(params...);`).
   - Indirect calls through C function pointers (`fn "C"`, including custom allocator vtable entries in [ADR-0029](../adr/0029-slice-and-map-allocator-word.md)) emit typed C function-pointer calls (`V076` restricts indirect C calls to scalar signatures until `P07.08`).
   - C-callable Go++ definitions (`@Export("C")` functions and `_GPTC...` callback wrappers) are functions of GIR with `CallConv::C` that the lowering makes: their bodies forward arguments to and return results from the underlying `__attribute__((swiftcall))` Go++ function ([calling-convention.md](calling-convention.md)). An `@Export("C")` function has external linkage under its declared name; a `_GPTC...` wrapper is made by every package that gives the function to C, and the linker keeps one.
   - An `@Extern("C")` function used as a Go++ `func` value is called through its `_GPTE...` code, which takes the environment first and calls the function as C does.

4. **The names of the types are Go++'s own.** A struct that `gopp` writes has the layout of the C struct on the other side and a name of its own (`T12`, fields `f0`, `f1`), which is all the ABI asks for. The rules of C for types of two translation units ask for the same tag and the same member names too, so a check that compares function types by name sees another type: under `-fsanitize=function` or a CFI check of indirect calls, C code that calls a Go++ function through a pointer whose type takes or gives such a struct by value is reported. For the same reason a pointer parameter of such a function is `T *`, never `const T *`. A signature of scalars and of plain pointers to them is the same type on both sides. A struct that comes from an imported header is the header's own type ([ADR-0036](../adr/0036-generated-c-includes-headers.md)) and has no such difference.

## Verification

- **Unit tests (`crates/gopp-gir`, `crates/gopp-codegen-c`):** `CTypeRules` boundary tests (both sides of depth `64`, `4096` fields, `1 GiB`, values without a size, the order of the reasons, answers that do not depend on the order of asking, and linear step counts on types that double at each level), verifier `V067`/`V075`/`V076` cases, lowering tests for each refusal with the nearby declarations that cross, for `@Export("C")` and `_GPTC` functions, and an emitter test that compiles a unit with crossing structs for both targets with every warning as an error.
- **Round trips against C (`tests/run/cabi_*.gopp`, each with a `.c` file beside it that the same Clang compiles and links):** scalars of every width and sign (a narrow value read as an `int` on the other side), pointers, `Option[*byte]`, small, mixed, all-float, large, three-byte and nested structs, a struct with an array of length `0`, 24 parameters that exhaust the registers, `@Export("C")` functions that C calls with the same values, and callbacks. Each program checks what comes back itself. They run in the normal suite at `-opt=0` on `linux/amd64` and `windows/amd64` ([ADR-0037](../adr/0037-v1-targets.md)), and in a long test at `-opt=0`, `2` and `3`, as a module and as an executable.
