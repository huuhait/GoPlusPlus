# The internal calling convention

Go++ calling Go++ uses an internal register-rich convention decided in `crates/gopp-gir` (parameter pass modes and `CallConv`) and emitted by `crates/gopp-codegen-c` ([ADR-0001](../adr/0001-implementation-language.md), [ADR-0030](../adr/0030-amd64-calling-convention.md), [c-abi.md](c-abi.md)). On `amd64` and `arm64`, Go++ functions emit GNU C23 `__attribute__((swiftcall))` (with `__attribute__((swift_context))` for closure environments); on `riscv64` and `wasm32`, Go++ functions fall back cleanly to standard C prototypes; C functions always emit standard C prototypes.

## Three decisions

1. **How a parameter is handed over** (`pass_of`, GIR level, determined from the signature alone). It sets the GIR parameter type (`param_type`, shared by declarations, method signatures, and itab slots), so a call site and a declaration cannot disagree, and any `Func` pointer type carries the exact parameter types. Whether the copy of a by-value argument is made or skipped ([ADR-0041](../adr/0041-values-are-copied.md) rule 4) never changes the parameter type or the registers used.
2. **Which convention a call uses** (GIR level, carried on `Func` types as `CallConv::Go` or `CallConv::C`, printed `fn "C"(...)` for C). Every function's `CC` matches its signature (`V075`). Direct calls read the callee's convention; indirect calls read the function-pointer type's convention.
3. **How values travel in registers or memory** (`crates/gopp-codegen-c` -> Clang 23 lowering via `__attribute__((swiftcall))` and `swift_context`).

### 1. Pass modes (`gopp-gir`, PAR-10, [ADR-0032](../adr/0032-shared-buffers-and-borrow-engine.md))

| Parameter | Mode | GIR type of the parameter |
|---|---|---|
| Trivial (`OWN-C1`): scalars, raw pointers `*T`, structs and arrays of them | Value (copy) | The type itself; the callee owns its bitwise copy (`p.X = 1` leaves the caller's value untouched). |
| Can be copied, and the copy is more than the bytes (`OWN-C2`): `string`, `[]T`, `map[K]V`, `Unique[T]` over a `T` that can be copied, a type with `drop()` and `Clone() T`, `Shared[T]`, `Weak[T]`, `chan T`, `interface`, `func`, and structs/arrays/`Option`s of them (`s[i:j]` and `arr[i:j]` are `[]T` values and pass the same way, `OWN-S2`; a `&s[i:j]` view is a `*[]T` and passes as one pointer; an interface converted for a non-keeping parameter is passed as a borrowed `(itab \| 1, &copy)`, `TYP-7`) | Value (the callee's own copy, unless the copy is skipped, [ADR-0041](../adr/0041-values-are-copied.md)) | The type itself (`string` is 3 scalars `{ptr, len, backing}`, `[]T` is 4 scalars `{ptr, len, cap, backing}`, `map[K]V` is 1 scalar `gopp_map*`). A copy of a counted value is a retain; a copy of a `[]T` or `map[K]V` is a new buffer with a copy of every element or entry. **No copy and 0 retain/release at the call boundary** when the copy is skipped (`PAR-1`): the callee only reads the parameter and the argument cannot change during the call, so the argument is passed as it is and stays the caller's; or the argument is a temporary, a local on its last use, or `move(x)`, so the value itself is handed over and the callee owns it. Otherwise the callee gets a copy (when the argument is read from an aliasable place that the call could write, the copy is made before the call; for a counted value it is a temporary retain, `PAR-1`). The callee drops a parameter it owns at `Return` / unwind unless it is moved onward. |
| Cannot be copied (`OWN-C3`) value parameter: a type with `drop()` and no `Clone() T`, a `@Pinned` type, a C++ class without a usable copy constructor, or an aggregate containing one (except pure `@Operator` methods, `@Convert` methods, `Clone() T`, and `[]T` parameters the function only reads, which take `OWN-C3` operands as they are, read-only, without consuming them, `PAR-5`) | Value (moved, `PAR-5`) | The type itself; the argument is `move(x)` or a temporary (a plain `f(x)` is a compile error with fix-its `move(x)` and `&x`), ownership moves from caller to callee, and the callee drops it at `Return` / unwind unless moved onward. |

- **Open:** [ADR-0041](../adr/0041-values-are-copied.md) does not fix which side of a call makes a copy that is needed, or how a callee learns that an argument was handed over (`move(x)`, a temporary, a last use) so that it keeps the value instead of copying it. A call with no visible body (an interface method, a `func` value) has no "only reads" fact for its parameters. Both are settled with the checker and runtime work.
- **Method receivers:** Parameter `0` follows the exact same rule (`pass_of_recv`, `PAR-1`): a pointer receiver `*T` passes the pointer (`*[]T` and `*map[K]V` are ordinary pointer types, `OWN-P1`); a value receiver uses its category row in declarations, bodies, static calls, and itab thunks (an `OWN-C3` consuming value receiver cannot be called through an interface, `PAR-5`, whereas `Clone() T` borrows as `*T`). Through an interface (`TYP-5`, `TYP-9`), a held pointer (`&e`) is passed as-is, while a boxed or inline value is immutable all the way down (mutating a slice or map inside a box is rejected at compile time; a value-receiver method that writes to its slice or map receiver writes to its own copy, which it drops at return, leaving the box unchanged, `OWN-P9`, `PAR-1`).
- **Borrowing `OWN-C3`:** Borrowing a value that cannot be copied uses an explicit `*T` parameter or receiver (`Unique[T]` places convert implicitly to `*T`, `OWN-P6`), or one of the read-only forms of `PAR-5` (pure `@Operator`, `@Convert`, `Clone()`, a `[]T` parameter the function only reads).
- **Variadic forwarding:** `vf(g())` binds fixed parameters and packs the remaining results into the variadic slice.
- **C signatures:** `@Extern("C")` declarations and `@Export("C")` entry wrappers take every parameter by C value (C types hold no Go++ `string`, `[]T`, or `map[K]V`).
- **Underlying-type assignability & interface conversions:** Identical underlying types where one is unnamed (`map[string]int` passed to `type Tab map[string]int`) pass with identical scalars without allocation. Converting a mutable-container `OWN-C2` (`[]T`, `map[K]V`, or a struct containing one) into a **kept** interface is a copy like any other: the box gets its own copy, or the value itself in $O(1)$ on its last use or with `move(x)` (`TYP-6`, `TYP-7`). In a type switch or assertion (`TYP-9`), an `OWN-C1` type copies, an immutable `OWN-C2` type retains its handle, and a mutable-container `OWN-C2` or `OWN-C3` type binds a deep read-only borrow into the box (retaining the box across the scope when asserted from an aliasable place).
- **Complexity:** `holds_owned(type)` is iterative and memoized per type, proven linear on chains, diamonds, wide structs, and wide parameter lists.

### 2. The convention of a call (`gopp-gir`)

| Callee | Convention |
|---|---|
| Go++ function, method, itab thunk (`_GPTI...`), type-descriptor / map `eq` and `hash` glue, or pointer of a Go++ `Func` type | `CallConv::Go` (`__attribute__((swiftcall))` on `amd64`/`arm64`; C fallback on `riscv64`/`wasm32`) |
| `@Extern("C")` function, libc helper, `@Export("C")` thunk, `_GPTC...` callback wrapper, or program entry `main` | `CallConv::C` (standard C23 prototype, [c-abi.md](c-abi.md)) |
| Indirect C `Func` pointer (`fn "C"`, including [ADR-0029](../adr/0029-slice-and-map-allocator-word.md) allocator table entries) | `CallConv::C` (`V076`: scalars only until `P07.08`) |

After GIR lowering, `check_calls` verifies that every direct call matches its callee's `CallConv` and that every function stored in static data (itabs, method tables, descriptors) carries `CallConv::Go`.

### 3. Register transport (`gopp-codegen-c` -> Clang 23, [ADR-0030](../adr/0030-amd64-calling-convention.md))

On `amd64` and `arm64`, `gopp-codegen-c` annotates Go++ functions and function-pointer typedefs with `__attribute__((swiftcall))`, and closure / method-value code pointers with `__attribute__((swift_context)) void *_gopp_env`:

- **`amd64`:** Returns up to **4 integer scalars** (`rax`, `rdx`, `rcx`, `r8`) and **4 float scalars** (`xmm0`–`xmm3`) in registers; passes `__attribute__((swift_context))` in `r13`; preserves SysV/Win64 callee-saved registers across unwinding frames.
- **`arm64`:** Returns up to **8 integer scalars** (`x0`–`x7`) and **8 float scalars** (`v0`–`v7`) in registers; passes `__attribute__((swift_context))` in `x20`; preserves AAPCS64 callee-saved registers across unwinding frames.
- **`riscv64` / `wasm32`:** `TargetSpec::supports_swiftcall()` is `false`; `gopp-codegen-c` omits `swiftcall`/`swift_context` and passes `_gopp_env` as ordinary parameter `0`, using Clang 23's native target C ABI seamlessly.

| Value | Kind | C23 representation |
|---|---|---|
| `()` or zero-scalar aggregate | Ignore | `void` (result) / omitted |
| Scalar (`bool`, integer, float, pointer, C `fn`) | Scalar | Native C23 scalar (`bool`, `int8_t`..`uint64_t`, `float`, `double`, `T*`) |
| Aggregate of `<= 4` scalars (`string` = 3 words, `[]T` = 4 words, `gopp_iface` = 2 words, `gopp_func` = 2 words, `[1]T` flattened) | Direct | Flat scalar struct / scalar parameters in declaration order |
| Aggregate of `> 4` scalars | Indirect | `const T *` pointer to caller-owned argument slot |
| Multi-value / aggregate result fitting target register budget (`<= 4` int + `<= 4` fp on `amd64`; `<= 8` + `<= 8` on `arm64`) | Direct | Value-returned C23 `struct` under `__attribute__((swiftcall))` |
| Result exceeding target register budget (e.g. `(string, error)` = 5 words or `([]T, error)` = 6 words on `amd64`; stays in registers on `arm64`) | Indirect | Hidden first output pointer (`ret_t *_gopp_ret`) into caller temporary (`V066`) |

Because `(int, error)` is 3 integer words (`int64_t` + 2-word `gopp_iface`), it returns purely in registers on both `amd64` (`rax`, `rdx`, `rcx`) and `arm64` (`x0`, `x1`, `x2`), and `f!()` branches on the `itab` register directly without touching stack memory. On `arm64`, `(string, error)` (5 words) and `([]T, error)` (6 words) also stay completely in registers (`x0`–`x5`).

## C-ABI thunks

A C-ABI thunk is a `CallConv::C` function emitted by `gopp-codegen-c` whose body forwards to a `CallConv::Go` function:

- **`@Export("C") func f(...)`:** Exports an unmangled C symbol `f` with a standard C prototype that calls the mangled Go++ function (`_GP...`), so Go++ callers still use `swiftcall` while C callers use the C ABI. The exported C name must not start with `_GP` ([appendix A.8](../spec/appendix-a-symbol-names.md), `SYM-4`, `E0445`; `@Extern` reserved prefix is `E0442`), must name `"C"`, must have a body, cannot combine with `@Extern`, cannot be `main`, and must use valid C types (`V067`).
- **Callbacks (`_GPTC...`):** Passing a top-level Go++ function to an `@Extern("C")` callback parameter (`fn "C"`) emits one `static inline` / weak ODR wrapper `_GPTC<mangled>` per target function so every translation unit hands C a valid C-ABI function pointer.
- **Verifier invariants:** `V067` (all `CallConv::C` declarations, thunks, and callback types use C-compatible types), `V075` (every function's `CC` equals its signature's `CC`), `V076` (indirect `fn "C"` calls pass scalars only), `V078` (`hasEnv` functions carry `CallConv::Go` and `*u8` env slot `0`).

## Integration across features

| Feature | How it plugs in |
|---|---|
| Interface itab calls (`P04.06`) | Itab slots hold `CallConv::Go` function pointers built with `pass_of`; `_GPTI...` thunks receive `data` first and forward according to `pass_of_recv`. |
| Closures & func values (`P04.07`) | `gopp_func` is `{code, env}`; `code` is `__attribute__((swiftcall))` taking `__attribute__((swift_context)) void *_gopp_env` (`r13`/`x20`). `@Extern("C")` functions used as Go++ `func` values get a `_GPTE...` Go-convention adapter that calls C. |
| Monomorphized generics (`P04.08`) | `pass_of` and `pass_of_recv` run on substituted instance types (`T = string` -> 3 scalars, `T = int` -> 1 scalar, `T = map[string]int` -> 1 pointer, `T = Unique[Node]` -> 1 pointer; each is the callee's own copy unless the copy is skipped; a `T` that cannot be copied is moved in). |
| Fallible `!` calls (`P04.10`) | `(T, error)` returns directly in `swiftcall` registers when `<= 4` words on `amd64` (`<= 8` on `arm64`); `!` tests `res.err.itab != NULL` directly. |
| Drops & unwinding (`P04.09`, `P04.26`) | Callee drops `OWN-C3` value parameters (and any `OWN-C2` parameter it owns) via `__attribute__((cleanup(...)))` / GIR `Drop` paths; `swiftcall` preserves standard C callee-saved unwind registers. |
| Goroutine entry (`P12`) | `go f(...)` spawns via a thin C-callable runtime entry wrapper `void (*)(void *)` invoking the `swiftcall` target. |

## Aliasable arguments and `nil` maps

When an `OWN-C2` argument is read from an aliasable place that the callee could overwrite mid-call (`u.SetName(u.Name)`, `s.fail(s.err)`, a slice element, a map entry, `*p`, or a local whose `&` is also passed to the same call), its copy is not skipped unless the compiler can show that the call does not write that place ([ADR-0041](../adr/0041-values-are-copied.md) rule 4, `PAR-1`): the caller makes the copy before the call. For a counted value (`string`, `Shared[T]`, an interface box) the copy is a temporary retain held across the call, so an intermediate drop cannot free the backing before the callee reads it (`PAR-1`, [ADR-0032](../adr/0032-shared-buffers-and-borrow-engine.md)); for a `[]T` or `map[K]V` it is a copy of the buffer. Because a `map[K]V` parameter is the callee's own copy, `m[k] = v` on an addressable local, struct field, slice element, or map entry initializes a `nil` map in place via `&slot`, whereas passing a map by value never changes the caller's variable (passing a provably `nil` map to a void/non-returning function emits a compile-time warning, `OWN-P9`, `PAR-1`). A function that fills the caller's map takes `*map[K]V`; a write to a by-value slice or map parameter that is then lost is a compile error ([ADR-0041](../adr/0041-values-are-copied.md) rule 9).

## Verification

- **Codegen & assembly tests (`crates/gopp-codegen-c`, `tests/codegen/`):** Verify `__attribute__((swiftcall))` and `__attribute__((swift_context))` emission and Clang-23 assembly register allocation (`rax, rdx, rcx` on `amd64` and `x0..x5` on `arm64` for `(T, error)` returns without stack spills).
- **GIR & verifier tests (`crates/gopp-gir`):** Pass-mode snapshots (`OWN-C1`; `OWN-C2` copied, passed without a copy, and handed over; moved vs borrowed `OWN-C3`), C-ABI callback/export thunks, `V067`/`V075`/`V076`/`V078`, and linear step-count stress tests.
- **Differential execution (`tests/run/cc-*`, `cabi-reverse-*`):** Generated 24-parameter / multi-return Go++-to-Go++ call hierarchies and C-to-Go++ reverse callback/export suites verified across all `-opt` levels on `amd64` and `arm64`.
