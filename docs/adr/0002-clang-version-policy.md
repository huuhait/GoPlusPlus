# ADR-0002: One pinned Clang/LLD major (23) and Rust toolchain from a pinned nixpkgs

- **Status:** accepted
- **Date:** 2026-11-11
- **Plan reference:** D0.2, D0.3, D0.4, P00.03, [ADR-0001](0001-implementation-language.md)

## Context

`gopp` is written in **100% Rust** and emits **Clang-23 GNU C23** (`.c`, or
`.cpp` `-std=c++20` when importing C++ headers), invoking `clang` and `lld`
(`-fuse-ld=lld`) as its backend compiler and cross-linker, and loading
`libclang` on demand via `clang-sys` (`runtime` feature) when importing C
headers (`P07`).

GNU C23 attributes (`swiftcall`, `swift_context`, `cleanup`), optimizer behavior,
DWARF 5 output, and cross-linking flags vary across compiler majors, so Go++
pins exactly one supported Clang/LLD major alongside the Rust toolchain in
`flake.nix`.

Pinned versions in `nixpkgs` (`nixos-26.05`):

| Tool | Version | Role |
|---|---|---|
| `rustc` / `cargo` | `1.93.0` (Edition 2024) | Compiles the `gopp` Rust workspace |
| `clang` / `clang++` | `23.1.0` | Compiles emitted GNU C23 (`.c`) and C++20 (`.cpp`), dumps C++ ASTs (`P08`) |
| `lld` (`ld.lld`, `ld64.lld`, `lld-link`, `wasm-ld`) | `23.1.0` | Cross-links ELF, Mach-O, COFF/PE, and Wasm targets (`-fuse-ld=lld`) |
| `libclang` | `23.1.0` | Loaded at runtime by `gopp-cimport` for C header import (`P07`) |

All pinned packages are served from the Nix binary cache, so `nix develop`
builds nothing from source.

## Decision

- Pin **Clang 23 & LLD 23 (`23.1.0`)** as the backend C23/C++20 compiler,
  cross-linker, DWARF verifier, and `libclang` provider.
- Pin the **Rust Edition 2024** toolchain (`rustc` / `cargo` >= 1.93.0) in
  `flake.nix` and `Cargo.lock`.
- `gopp-codegen-c` checks `clang --version` on discovery (`gopp host-toolchain`)
  and requires Clang major **23**.
- Upgrade the pinned Clang major at most once per Go++ minor release in a
  dedicated change that runs the full workspace and optimization-matrix test
  suites; Clang flag generation lives solely in `crates/gopp-codegen-c` and
  `crates/gopp-cimport`.
- Configure `sccache` (`RUSTC_WRAPPER=sccache`, 100 GiB cache in
  `~/.cache/sccache`, shared across all git worktrees) for both Rust crate
  builds and Clang C/C++ compilations.

## Consequences

- Every developer and CI runner gets identical `rustc`, `cargo`, `clang` 23.1.0,
  and `lld` 23.1.0 binaries from `flake.lock`.
- Release `gopp` builds with `cargo build --release` with zero C/C++ compiler
  libraries linked into `gopp` itself.
- Cross-compilation (`-target=linux/arm64`, `linux/riscv64`, `darwin/arm64`,
  `windows/amd64`, `wasip1/wasm32`, etc.) uses the single pinned `clang` 23.1.0
  + `lld` 23.1.0 binary (`--target=<triple> -fuse-ld=lld`) plus a target sysroot
  when linking executables (`docs/design/targets-and-linking.md`).
- Non-Nix users install Rust (`>= 1.93`) plus Clang/LLD 23 (`GOPP_CLANG` / `PATH`).
