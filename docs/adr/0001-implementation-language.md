# ADR-0001: The compiler is written in 100% Rust and emits Clang-23 C23 / C++20

- **Status:** accepted
- **Date:** 2026-11-11
- **Plan reference:** D0.1, P04, P07, P08

## Context

Go++ compiles to native machine code across multiple architectures, enforces
compile-time ownership, borrow lending, and race-free concurrency on a typed
mid-level IR (**GIR**, `docs/design/gir.md`), supports external middle-IR pass
plugins (`-fpass-plugin`), and interoperates directly with C and C++ headers
(P07, P08).

Clang 23 in GNU C23 mode (`-std=gnu23`) exposes every backend primitive Go++
needs directly from generated C source:

- `__attribute__((swiftcall))` and `__attribute__((swift_context))` for
  multi-word register returns and closure environment registers (`ADR-0030`).
- `-fexceptions` + `__attribute__((cleanup(fn)))` for zero-cost stack-unwinding
  panic cleanups and `defer`.
- `-fwrapv -fno-strict-aliasing -ffp-contract=off -fno-builtin-malloc` +
  `__builtin_fma` for exact Go integer wraparound, pointer aliasing rules,
  allocation OOM semantics, and deterministic `arm64` FMA fusion
  (`docs/design/pipelines.md`).
- Native C structs and prototypes for exact target C ABI layout and register
  passing (`docs/design/c-abi.md`), plus `clang-23 --target=<triple>
  --sysroot=<path> -fuse-ld=lld` for multi-architecture cross-compilation and
  cross-linking (`docs/design/targets-and-linking.md`).
- When a package imports C++ headers (`P08`), `gopp` queries `clang++` 23
  (`-std=c++20 -Xclang -ast-dump=json` and layout probes) for overload
  resolution, template instantiation, record layouts, and vtables, and emits a
  `.cpp` translation unit (`-std=c++20 -fno-rtti -fexceptions`) for that
  package.

## Decision

1. **100% Rust compiler (`gopp`, Rust Edition 2024).** Every stage — lexing,
   recursive-descent + Pratt parsing, name resolution, type/ownership/borrow/
   concurrency checking, GIR construction, GIR verification, middle-IR pass
   hosting, C23/C++20 emission, build caching, and the CLI driver — is written
   in Rust inside a Cargo workspace (`crates/gopp-*`).
2. **Clang-23 C23 backend (`.c` default, `.cpp` on C++ import).** `gopp-codegen-c`
   lowers verified GIR to deterministic Clang-23 GNU C23 source and invokes the
   pinned `clang` 23.1.0 + `lld` 23.1.0 toolchain (`ADR-0002`) to emit object
   files, shared libraries, or linked executables for any supported target.
3. **Middle-IR plugins (`-fpass-plugin=<path>` and `-fplugin-arg-<name>-<key>=<val>`).**
   `crates/gopp-plugin-api` defines a `#[repr(C)]` `abi_stable::StableAbi`
   plugin interface (`GirPassPlugin`) with two pipeline stages:
   - `PreDropElab`: runs after mandatory `MoveCheck`, `BorrowCheck`, and
     `ConcurrencyCheck` and before drop elaboration.
   - `PostDropElab`: runs after drop elaboration and before Clang-23 C23
     emission.
   Every plugin pass is followed immediately by `gir::verify_module`
   (`V001`–`V085`); any broken invariant halts compilation with `E0440` naming
   the offending plugin. Plugin binaries and `-fplugin-arg-*` flags are hashed
   with `blake3` into the `.gopx` and build cache keys. Optional `.wasm` plugins
   run inside a deterministic `wasmtime` sandbox exchanging MessagePack
   (`rmp-serde`) GIR payloads.
4. **Cargo crate stack (pinned to latest stable versions).**

| Layer | Crate | Version | Role |
|---|---|---|---|
| Lexing | `logos` | `0.16.1` | Compile-time jump-table DFA lexer wrapped with Go semicolon insertion |
| Deep AST recursion | `stacker` | `0.1.25` | Dynamic stack growth for deeply nested expressions (up to 1024 levels) |
| Unicode identifiers | `unicode-ident` | `1.0.27` | UAX #31 identifier tables for Go++ identifiers |
| Diagnostics | `ariadne` | `0.6.0` | Multi-span color terminal compiler error renderer |
| Internal errors | `thiserror` | `2.0.21` | `std::error::Error` derives across compiler crates |
| Arena IDs | `la-arena` | `0.3.1` | Typed `Idx<T>` (`u32`) arenas for AST and GIR nodes |
| String interning | `lasso` | `0.7.3` | Multi-threaded string interner (`ThreadedRodeo` / `Spur` 4-byte IDs) |
| Fast maps/sets | `rustc-hash` | `2.1.3` | `FxHashMap` / `FxHashSet` for integer node and symbol IDs |
| Deterministic maps | `indexmap` | `2.14.2` | Insertion-ordered maps for deterministic C23 struct/function emission |
| Inline vectors | `smallvec` | `1.16.5` | Stack-inlined call arguments and basic-block edges |
| Bitflags & CFG sets | `bitflags` / `fixedbitset` | `2.13.2` / `0.5.9` | Compact IR attribute masks and dense dataflow/dominator bitsets |
| Exact constants | `num-bigint` | `0.5.1` | Exact 4096-bit `Num/Den` rationals and 512-bit `BigFloat` (`ADR-0009`) |
| Unification & CFG | `ena` / `petgraph` | `0.14.4` / `0.8.3` | Union-find unification tables and CFG dominator/SCC analysis |
| Targets & objects | `target-lexicon` / `object` | `0.13.5` / `0.41.0` | Target triple layout rules and pure-Rust ELF/Mach-O/COFF/Wasm inspection |
| C header import | `clang-sys` | `1.9.1` | Runtime-loaded `libclang` 23.1.0 bindings (`runtime` feature) for P07 C headers |
| Native IR plugins | `abi_stable` / `libloading` | `0.11.3` / `0.9.0` | `#[repr(C)]` ABI-checked trait objects and `.so`/`.dylib`/`.dll` plugin loading |
| Serialization & cache | `serde` / `serde_json` / `postcard` / `rmp-serde` / `zstd` / `blake3` / `xxhash-rust` | `1.0.229` / `1.0.154` / `1.1.3` / `1.3.1` / `0.14.3` / `1.8.7` / `0.8.19` | Deterministic `.gopx` GIR containers (`postcard` + `zstd`), JSON protocols, Wasm plugin wire format, `blake3` cache keys, and `xxh3` hashes |
| Parallelism | `rayon` | `1.12.0` | Parallel per-function checking, GIR pass execution, and C23 emission |
| C23 number formatting | `itoa` / `ryu` | `1.0.19` / `1.0.23` | Zero-allocation integer and exact floating-point literal formatting |
| CLI & driver | `clap` / `which` / `tempfile` | `4.6.7` / `8.0.6` / `3.27.0` | CLI driver flags, `clang-23`/`lld` discovery, and atomic temporary `.c`/`.o` files |
| Testing & benches | `insta` / `proptest` / `divan` | `1.49.0` / `1.12.0` / `0.1.21` | Snapshot tests for AST/GIR/C23, property-based pass equivalence, and throughput benchmarks |

Ecosystem crates for optional Wasm plugins, external plugin repositories, and Phase-3 IDE support:
- **Sandboxed `.wasm` `-fpass-plugin` host:** `wasmtime = "50.0.1"`, `rmp-serde = "1.3.1"`.
- **External IR pass plugin repositories:** `egglog = "3.0.0"` (equality saturation / e-graphs), `rand = "0.10.3"`, `rand_chacha = "0.10.0"`.
- **Phase-3 IDE / Language Server (`P14`):** `salsa = "0.28.5"`, `lsp-server = "0.10.0"`, `lsp-types = "0.97.0"`, `tower-lsp-server = "0.23.0"`, `tokio = "1.54.1"`.

## Consequences

- **Memory safety by default.** Front-end, checker, GIR, and C23-codegen crates
  forbid `unsafe` (`#![forbid(unsafe_code)]`); `unsafe` is confined strictly to
  dynamic library loading (`gopp-plugin-host`, `gopp-cimport`) and executor
  process sandboxing (`test-exec`).
- **Fast, self-contained compiler builds.** `gopp` builds with `cargo build` and
  links only host system libraries (`libc`, `libdl`, `libpthread`).
- **Exact multi-arch C ABI and cross-compilation.** `clang-23 --target=<triple>
  -fuse-ld=lld` compiles and cross-links all 8 supported targets (`linux`,
  `darwin`, `windows`, `wasip1`) consistently.
- **Extensible middle end.** External `-fpass-plugin` passes inspect and rewrite
  typed GIR while `MoveCheck`/`BorrowCheck`/`ConcurrencyCheck` and
  `gir::verify_module` guarantee Go++ safety invariants.
