# Go++

Go++ is a Go-like language without a garbage collector, built on LLVM, with
first-class C and C++ interop. The compiler is `gopp`.

Status: phase P00 (foundations). `gopp version` and `gopp host-toolchain` work;
there is no compiler yet.

## Where things are

- The plan: [plans/2026-10-27-goplusplus-language/](plans/2026-10-27-goplusplus-language/README.md)
- Decisions: [docs/adr/](docs/adr/)
- How to contribute: [CONTRIBUTING.md](CONTRIBUTING.md)
- Layout: `compiler/` (libraries and the `gopp` tool), `runtime/`, `std/`,
  `tests/`, `bench/`, `tools/`, `docs/`

## Build

You need [Nix](https://nixos.org/) with flakes. The dev shell provides LLVM 23,
Clang, LLD, CMake, Ninja, sccache, mold, lit, FileCheck, Go and GoogleTest.

```sh
nix develop -c cmake -S . -B build -G Ninja
nix develop -c ninja -C build gopp
nix develop -c build/bin/gopp version
nix develop -c ninja -C build check format-check
```

Without Nix: install LLVM, Clang and LLD 23, CMake, Ninja, lit, FileCheck and
GoogleTest (Debian/Ubuntu: `llvm-23-dev libclang-23-dev liblld-23-dev
llvm-23-tools clang-format-23` from apt.llvm.org plus `cmake ninja-build mold
sccache python3-lit python3-psutil libgtest-dev libc6-dev`; macOS:
`brew install llvm lld cmake ninja googletest sccache`),
then pass `-DLLVM_DIR=... -DClang_DIR=... -DLLD_DIR=...` to CMake.

Build types: `Dev` (default; `-O1 -g`, assertions on, LLVM and Clang linked as
shared libraries with mold, for fast rebuilds) and `Release` (`-O3`, LLVM,
Clang and LLD linked statically). The dev shell turns on sccache with a 100 GB cache
(`SCCACHE_CACHE_SIZE`, shared by all build folders); CMake uses it
automatically (ccache is the fallback).

## License

Apache-2.0 WITH LLVM-exception. See [LICENSE](LICENSE).
