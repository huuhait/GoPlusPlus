# ADR-0002: One pinned LLVM major (23) from a pinned nixpkgs

- **Status:** accepted
- **Date:** 2026-10-27
- **Plan reference:** D0.2, D0.3, D0.4, P00.03

## Context

`gopp` links LLVM, Clang and LLD. Their C++ APIs change every major release,
so we want exactly one supported major at a time. The dev environment is a Nix
flake (D0.4), so the version is whatever the pinned nixpkgs provides. The plan
says: use the newest stable LLVM major in the pinned nixpkgs, at least 21.

Checked with `nix eval` on nixpkgs `nixos-26.05` at the revision in
`flake.lock` (718f670e):

| Package set | Version | Notes |
|---|---|---|
| `llvmPackages` (default), `llvmPackages_21` | 21.1.8 | the clang already installed on the owner's machine |
| `llvmPackages_22` (= `llvmPackages_latest`) | 22.1.8 | last patch release of the 22 series |
| `llvmPackages_23` | 23.1.0 | official release (`officialRelease`, tag `llvmorg-23.1.0`) |
| `llvmPackages_git` | 24.0.0-unstable | development snapshot, not stable |

LLVM 21, 22 and 23 (LLVM, Clang, LLD, clang-tools) are all in the binary
cache, so the normal dev shell builds nothing.

## Decision

- Pin **LLVM 23** (`llvmPackages_23`, 23.1.0): the newest stable major in the
  pinned nixpkgs. 23.1.0 is LLVM's first final release of the 23 series (LLVM
  numbers final releases X.1.0, X.1.1, ...; release candidates are tagged
  `-rcN`). The 22 series is finished at 22.1.8, so pinning 23 also keeps us on
  the series that still receives patch releases.
- The pin lives in two places that must change together: `llvmMajor` in
  `flake.nix`, and `flake.lock` (nixpkgs revision). CMake refuses LLVM older
  than 21.
- Upgrade at most once per Go++ minor release, in a dedicated change that runs
  the full suite. LLVM/Clang API use stays inside `lib/CodeGen`, `lib/Import`,
  `lib/Build` and the LLVM bridge of `lib/GIR`, so an upgrade touches a bounded
  area.
- nixpkgs LLVM is built with RTTI, so `gopp` is too. CMake reads
  `llvm-config --has-rtti` rather than assuming (D0.3). No exceptions.
- **Linkage (D0.2, D0.13).** Dev builds link `libLLVM.so` and
  `libclang-cpp.so` (fast relinks). Release builds link LLVM, Clang and LLD
  statically: `gopp` has no `libLLVM`/`libclang-cpp` dependency. nixpkgs builds
  LLVM with `LLVM_LINK_LLVM_DYLIB`, so its exported Clang and LLD CMake targets
  name `LLVM` (the shared library) as their LLVM dependency, although the
  archives themselves are ordinary object files. For a static build,
  `cmake/modules/GoppLLVM.cmake` points that dependency at LLVM's static
  component archives instead, so exactly one static copy of LLVM ends up in
  `gopp` (two copies would register every `cl::opt` twice). It leaves out a
  few tool-only components that would pull extra system libraries (libpfm,
  libffi, Polly, libxml2) and the tool-support archives that were themselves
  linked against `libLLVM.so`. CMake prints which linkage it chose.
  LLD has no shared library in nixpkgs, so its archives are used in both modes.
- The assertions-enabled shell (`nix develop .#assertions`) sets
  `LLVM_ENABLE_ASSERTIONS=ON` for LLVM, Clang and LLD together through
  nixpkgs' `devExtraCmakeFlags` hook (nixpkgs has no assertions switch; the
  three must agree because assertions change class layouts and add an
  ABI-breaking-checks symbol). The LLVM build type stays Release (a Debug LLVM
  is about 10x slower); only assertions are switched on. LLVM's own test suite
  is not run (`doCheck = false`), and its benchmarks are not built
  (`LLVM_INCLUDE_BENCHMARKS=OFF`: with assertions, LLVM 23 defines
  `_LIBCPP_HARDENING_MODE`, which clashes with the Nix compiler wrapper, and
  the vendored google-benchmark builds with `-Werror`). `gopp` itself is
  compiled by the normal clang; building it against an assertions LLVM keeps
  `NDEBUG` off (`cmake/modules/GoppBuildOptions.cmake`).
- The compiler cache is sccache (owner decision, replacing the plan's
  ccache): CMake uses sccache, else ccache, else none. The dev shells default
  to a 100 GB cache in `~/.cache/sccache`, shared by all worktrees.

## Consequences

- Everyone, CI included, gets the same LLVM from `flake.lock`.
- The assertions shell compiles LLVM, Clang and LLD from source on first use
  (cached in the Nix store afterwards; CI caches the store).
- A release `gopp` is self-contained apart from libc, libstdc++, libz and the
  other system libraries listed by `readelf -d`.
- Relying on the shape of nixpkgs' exported CMake targets is a coupling: if a
  future nixpkgs changes it, the static link breaks loudly at configure or
  link time (the Release CI job catches it).
- Non-Nix users must install LLVM 23 themselves and pass `-DLLVM_DIR`,
  `-DClang_DIR`, `-DLLD_DIR`.

## Alternatives

- **LLVM 22.1.8** (nixpkgs' `llvmPackages_latest`): a mature patch release,
  but not the newest stable major, and its series has ended.
- **LLVM 21** (nixpkgs default, already installed): one more major behind.
- **Release links Clang and LLD statically but LLVM as `libLLVM.so`**: simpler
  CMake, but not the static binary D0.2 asks for.
- **A custom static LLVM build** (`LLVM_LINK_LLVM_DYLIB=OFF`): a second large
  from-source build for every developer and CI run, for the same result.
- **System LLVM instead of Nix**: differs per machine; rejected by D0.4.
