# Targets and linking

Go++ cross-compiles from any host to every supported target out of the box using `crates/gopp-target` (`TargetSpec`, `TargetLayout`, backed by `target-lexicon` `0.13.5`) and `crates/gopp-codegen-c` (`ClangDriver`, invoking `clang` `23.1.0` + `lld` `23.1.0`, [ADR-0001](../adr/0001-implementation-language.md), [ADR-0002](../adr/0002-clang-version-policy.md)).

## Targets

One table, `TargetSpec` (`crates/gopp-target`), owns all target properties; nothing else in the compiler constructs a target triple or guesses pointer width, endianness, or C `long` size.

| Go++ `-target` | Clang `--target` triple | Ptr / Endian / C `long` | `swiftcall` | LLD flavor (`-fuse-ld=lld`) | Object format |
|---|---|---|---|---|---|
| `linux/amd64` | `x86_64-unknown-linux-gnu` | 64-bit, LE, 8B (LP64) | yes | `ld.lld` (`elf_x86_64`) | ELF |
| `linux/arm64` | `aarch64-unknown-linux-gnu` | 64-bit, LE, 8B (LP64) | yes | `ld.lld` (`aarch64linux`) | ELF |
| `linux/riscv64` | `riscv64gc-unknown-linux-gnu` | 64-bit, LE, 8B (LP64) | C fallback | `ld.lld` (`elf64lriscv`) | ELF |
| `darwin/arm64` | `aarch64-apple-darwin` | 64-bit, LE, 8B (LP64) | yes | `ld64.lld` | Mach-O |
| `darwin/amd64` | `x86_64-apple-darwin` | 64-bit, LE, 8B (LP64) | yes | `ld64.lld` | Mach-O |
| `windows/amd64` | `x86_64-pc-windows-msvc` | 64-bit, LE, 4B (LLP64) | yes | `lld-link` | COFF/PE |
| `windows/arm64` | `aarch64-pc-windows-msvc` | 64-bit, LE, 4B (LLP64) | yes | `lld-link` | COFF/PE |
| `wasip1/wasm32` | `wasm32-wasip1` | 32-bit, LE, 4B (ILP32) | C fallback | `wasm-ld` | Wasm |

- **Supported in version 1 ([ADR-0037](../adr/0037-v1-targets.md)):** `linux/amd64` and `windows/amd64`. Both pass every test. The other six rows stay in the table so that no code assumes one platform, but they are not tested or promised until they are decided after version 1; a build for one of them says so.
- **Target selection & deterministic CPU baseline:** `gopp build -target=windows/amd64` selects the target; without the flag, `GOPPOS` and `GOPPARCH` each stand in for that part of the host ([ADR-0005](../adr/0005-naming.md)); the default is the host. `gopp targets` prints the table. Unless a profile explicitly sets `cpu` ([ADR-0007](../adr/0007-build-profiles.md)), the CPU baseline is fixed per target (`-march=x86-64` on `amd64`, `-march=armv8-a` on `arm64`, `-march=rv64gc` on `riscv64`), never `-march=native`, so builds are byte-for-byte deterministic across machines.
- **Pure-Rust object inspection:** Object files, symbol tables, undefined-symbol allowlists, and embedded `.gopx` sections are inspected in pure Rust via `object` `0.41.0` across ELF, Mach-O, COFF/PE, and Wasm.
- **Compile-time layout guards:** Every generated C23 translation unit starts with `_Static_assert` checks verifying pointer width, `int64_t` alignment, `gopp_str` size (3 words), `gopp_slice` size (4 words), and `gopp_iface` size (2 words) against `TargetLayout`.

## Compilation and deterministic linking (`ClangDriver`)

`gopp-codegen-c` verifies the GIR module (`gopp_gir::verify_module`), emits deterministic GNU C23 (`.c` with `-std=gnu23`, or `.cpp` with `-std=c++20 -fno-rtti` when importing C++ headers), and invokes `clang` `23.1.0` with `--target=<triple> -fuse-ld=lld`:

- **Mandatory semantic flags:** `-fwrapv -fno-strict-aliasing -ffp-contract=off -fno-builtin-malloc -fno-builtin-calloc -fno-builtin-realloc -fno-builtin-free`, plus `-fexceptions -fasynchronous-unwind-tables` (or `-fno-exceptions` under `-panic=abort`).
- **Deterministic binaries:** On ELF targets, `ClangDriver` passes `-Wl,--eh-frame-hdr -Wl,--hash-style=gnu -Wl,--build-id=none -Wl,-z,relro` and strips transient temp paths via `-ffile-prefix-map=<tmpdir>=.`, making compiled objects and linked executables pure functions of their inputs.
- **Entry point:** The generated C entry function `int main(int argc, char **argv)` calls `_gopp_rt_init(argc, argv)`, gives the names of the functions of the unit to `_gopp_rt_funcs` (a trace reads them), runs package `init` functions in topological order, calls `main.main`, and returns `0`.
- **The packages of a program (`gopp_build::load`, `P06.01`):** the imports of a package are found in the standard library, the folder `std` of what `GOPPROOT` names or of the sources that the compiler was built from; an import that is not there is still a stand-in whose names are all taken to exist, and nothing with one is built. The files of a package are chosen by their names (`_linux`, `_amd64`, no `_test`) and by `@Build`, the packages are parsed wave by wave and side by side, an import cycle is `E0106`, and each package comes after those that it imports. They are checked in that order with one table of types and lowered side by side, each to a module and a unit of C of its own. A function, a variable or a method of another package is a declaration in the unit that names it (`declare global` in GIR). The unit of an imported package has the function `gopp.start.<import path>`, which gives the names of its functions to the runtime, gives its variables their first values and runs its `init` functions; the entry function of the program calls these in the order of the packages before the start of its own package (`INI-1`). Not there yet: a generic function or type of another package (`E0439`, `P06.05`), `.gopx` files, the cache of built packages and the modules of `gopp.mod`.
- **What several units each have** (the descriptor and the glue of a type, an instance of a generic function) is `GOPP_ONCE` or `GOPP_ONCE_DATA` in a unit: weak where objects are ELF; on Windows `inline` for a function and `__declspec(selectany)` for data, because the linker of COFF refuses two weak definitions. Data that is written further down in the unit is declared `extern` first, and its definition has the mark.
- **The standard library (`std/`, `P05.13`):** written in Go++. `internal/sys` is the one package that reaches the operating system: it declares functions of the runtime with `@Extern("C")` (`gopp_sys_write` in `runtime/os_linux.c` and `runtime/os_windows.c`), which a call reaches by the name with the convention of C; such a function takes and gives numbers, bools and pointers only, until C interop (`P07`). `fmt` has `Print` and `Println` for strings, bools, integers, errors, values with a `String` method and nil; the sample `01-hello` is built from it and prints what its file says.
- **The runtime:** an executable is linked with the objects of the runtime, which `gopp` builds once for a target and its flags and keeps in the cache ([runtime.md](runtime.md)).
- **The unit (`emit_c23`):** one `.c` file per package that includes no header, so `-emit=c` and `-emit=obj` need no sysroot. In order: the text of the header of the runtime (the scalar names from Clang's own macros, `__INT64_TYPE__` and the like, the layouts that both sides read, the functions of the runtime; [runtime.md](runtime.md)); the helpers that keep Go's results (`gopp_shl_*`, `gopp_shr_*`, `gopp_div_*`, `gopp_rem_*`, `gopp_f64_to_i32` and the like) as `static inline`; the layout checks; every type with parts as a struct `T<n>` (an array is a struct around a C array, a value that takes no room is not written at all), each followed by a `_Static_assert` of its size and alignment against `TargetLayout`; strings; globals; a prototype for every function with its symbol as an `__asm__` label, so a symbol may be any text; then the bodies, written in parallel and joined in order. A block is a label and its end a `goto`; a failed `Assert` calls `_gopp_panic_<kind>` of the runtime. The text is the same for any number of threads.
- **Not written yet:** cleanup blocks, `Drop` and `Rec` types are refused with `E0441` until their tasks; Go++ functions use the C convention until `swiftcall` (`P04.16`); the unit has no `#line` directives until debug info (`P04.12`).
- **A wrapped Clang:** `ClangDriver` runs Clang in the directory of the unit, with `NIX_HARDENING_ENABLE` empty and `NIX_CFLAGS_COMPILE` unset, so that the wrapper of Nix adds no flag of its own, and with `-Wno-unused-command-line-argument`.

### Sysroot and C library discovery

- **Host target:** `probe_host_toolchain` discovers `crt1.o`, `crti.o`, `crtn.o`, `libc`, and the dynamic linker via `clang -print-file-name=...` (supporting both standard FHS distributions such as Debian and store-based NixOS paths).
- **Cross targets:** `--sysroot=<dir>` / `-crt-dir=<dir>` supplies the target sysroot containing startup objects and `libc` (`libc.so` or `libc.a`), with `-dynamic-linker=<path>` overriding the target table's default FHS loader path when needed. `-emit=c` and `-emit=obj` succeed for every supported target even without a target sysroot installed.
- **Pre-link validation:** `check_link_inputs` validates every startup object, library search directory, and loader path before invoking `lld`. Missing sysroot pieces produce a structured `ariadne` diagnostic naming the exact missing file and the `-crt-dir` / `--sysroot` flag (`E0441`), never a raw linker crash.

## Cross-target verification matrix

Version 1 tests two targets ([ADR-0037](../adr/0037-v1-targets.md)). The other targets, and the `qemu-aarch64` setup that the dev shell still carries, are taken up after version 1. There is no CI yet: both columns are run by hand before a change is done.

| Check | `linux/amd64` (host) | `windows/amd64` |
|---|---|---|
| C23 emission (`-emit=c`) & object generation (`-emit=obj`, inspected via `object` `0.41.0`) | `cargo test` + `insta` snapshots | The same tests on the Linux host: emitting C and a COFF object needs no Windows sysroot |
| Linking | Host sysroot (`tests/run`) | On the Windows machine, with `lld-link` and the Microsoft C runtime |
| Execution | Native (`gopp test-run` + real binaries) | Real executables on the Windows machine; the fast runner is Linux-only ([test-runner.md](test-runner.md)) |
| FHS portability (non-Nix) | Debian container verification (`GOPP_TEST_DEBIAN_IMAGE`) | Does not apply |

## Release compiler binary

`cargo build --release -p gopp-cli` produces a single self-contained native `gopp` Rust binary with zero static C++ archive dependencies. At runtime `gopp` locates `clang` `23.1.0` and `lld` `23.1.0` via `GOPP_CLANG` or `which` (`8.0.6`), and loads `libclang` dynamically via `clang-sys` (`1.9.1`) only when compiling a package that imports a C or C++ header.
