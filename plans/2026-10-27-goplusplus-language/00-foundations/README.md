# P00 — Foundations

**Goal:** a buildable, tested, CI-checked skeleton for the `gopp` toolchain, plus the decisions that are expensive to change later: implementation language, LLVM policy, repository layout, names, license.
**Depends on:** — · **Unblocks:** every phase · **Size:** S · **Milestone:** prerequisite of M0

## Starting point (observed 2026-10-27)

- The project directory is empty and not yet a git repository.
- The machine runs NixOS 26.05 with 192 cores and 503 GiB RAM.
- Installed: clang 21.1.8 (Nix wrapper), CMake 4.1.2, Ninja 1.13.2, ccache 4.12.3, mold 2.40.4, Go 1.25.7, git 2.53.
- **Missing:** `llvm-config`, the LLVM/Clang development libraries (headers + libraries) and LLD. `gopp` must link against all of them, which P00.02/P00.03 handle.

## Decisions

**D0.1 — The compiler is written in C++20.**
*Why:* C++ interop (P08) needs Clang's C++ API: Sema for overload resolution and template instantiation, CodeGen for inline functions and ABI lowering, `ASTRecordLayout`, `MangleContext`. libclang's C API can't do any of that, and every other host language would need a large C++ shim anyway. LLVM's own APIs are C++-first.
*Rejected:* Rust (inkwell covers LLVM, but Clang Sema/CodeGen still need a C++ shim); Go (TinyGo shows LLVM via cgo works, but Clang's C++ API is out of reach); Zig.
*Later:* self-host in Go++ once P08 can import LLVM/Clang headers directly. That would be the strongest possible test of R7 (P16.08).

**D0.2 — One pinned LLVM major.** Use the newest stable LLVM major available in the pinned nixpkgs, at least 21 (clang 21.1.8 is installed). Upgrade at most once per Go++ minor release, in a dedicated change that runs the full suite. LLVM/Clang API use stays inside `lib/CodeGen`, `lib/Import`, `lib/Build` and the LLVM bridge of `lib/GIR`, so upgrades touch a bounded area. Release builds link LLVM, Clang and LLD statically into `gopp`; dev builds may use LLVM's shared library for faster relinks.

**D0.3 — Build the compiler with CMake + Ninja.** This is LLVM's own ecosystem (`find_package(LLVM)`, `find_package(Clang)`). Add ccache, and link with mold or lld. Match the pinned LLVM build: take the RTTI flag from `llvm-config --has-rtti` (nixpkgs LLVM 21 has RTTI on) and build without exceptions.

**D0.4 — The dev environment is a Nix flake.**
- `flake.nix` pins the LLVM/Clang/LLD dev packages, CMake, Ninja, ccache, mold, Python 3 + lit, Go and GoogleTest. `flake.lock` is committed, so everyone gets the same tools.
- An optional `.envrc` (`use flake`) lets direnv load the shell on `cd`; editors pick it up too.
- Scripts, CI and agents run commands as `nix develop -c <cmd>`.
- An optional second dev shell has an **assertions-enabled** LLVM/Clang/LLD of the same major: a Release build with assertions on, never a Debug build (about 10× slower). Misuse of Clang's C++ API must fail loudly during development. A CI job uses it too (D0.7).
- Non-Nix contributors get documented apt/brew equivalents now and a devcontainer later.

**D0.5 — Monorepo layout.**

```
GoPlusPlus/
  plans/              this plan
  docs/spec/          language specification (P01)
  docs/adr/           architecture decision records
  docs/design/        design notes (GIR, macros, …)
  compiler/
    include/gopp/      public headers, one dir per library
    lib/Basic         source manager, diagnostics, arenas, interning
    lib/Lex lib/Parse lib/AST lib/Format
    lib/Sema          types, checker, type-level ownership rules
    lib/GIR           mid-level IR: lowering, ownership flow checks, drops, mono, interpreter
    lib/CodeGen       GIR → LLVM IR, ABI, debug info, LLD driver
    lib/Import        Clang-based C/C++ importer
    lib/Macro         macro expansion
    lib/Build         build graph, scheduler, cache, export data
    lib/Mod           gopp.mod / gopp.sum, MVS, fetchers
    lib/CDep          cdep blocks, CMake bridge, sandbox
    tools/gopp         CLI entry point
  runtime/            runtime (Go++ + C + asm)
  std/                standard library (Go++)
  tests/              lit suites: lex, parse, sema, gir, codegen, run, interop, mod, cdep
  bench/              compile-speed and runtime benchmarks
  tools/              developer scripts
```

**D0.6 — Testing infrastructure.**
- LLVM `lit` + `FileCheck` for golden, diagnostic and IR tests.
- GoogleTest for unit tests.
- A `run` test format: build, execute, compare stdout and exit code.
- Differential tests against `go run` (P04.14).

**D0.7 — CI.** The code is hosted on GitHub, and CI runs on GitHub Actions (owner decision; decides Q1). Linux x86-64 and arm64 only for v0.1 (macOS and Windows after v0.1, see P16 D16.3). Jobs:
- Linux x86-64 Release.
- Linux x86-64 Debug+ASan/UBSan (sanitizing the compiler itself).
- Linux x86-64, building `gopp` against the assertions-enabled LLVM (D0.4).
- Linux arm64, from P04.

Every job runs `ninja check` and the format check.

**D0.8 — Names** (owner decision; decides Q3).
- The language is **Go++** (codename GoPlusPlus).
- Everything else is `gopp`: the CLI `gopp`, source files `.gopp`, manifests `gopp.mod`/`gopp.sum`, export data `.gopx`, directives `//gopp:…`, environment variables `GOPPCACHE`, `GOPPMODCACHE`, `GOPPPROXY`, `GOPPOS`, `GOPPARCH`, ….
- *Why `gopp`, not `gpp`:* Linux distributions already ship an unrelated `gpp` binary (the "generic preprocessor").
- *Known risk, tracked in ADR-0005:* "Go" is a Google trademark. The owner keeps the name Go++; a trademark check runs before the first public release (P16 D16.6).

**D0.9 — Decision records.** `docs/adr/NNNN-title.md`, each with context, decision, consequences and alternatives. This plan's decisions are the baseline; any change needs an ADR plus an update to the affected phase README.

**D0.10 — License: Apache-2.0 WITH LLVM-exception** (confirmed by the owner; decides Q2). It matches LLVM and includes a patent grant. The exception lets the runtime and std be linked into user binaries without attribution requirements.

**D0.11 — Code style.** LLVM coding standards, enforced by `.clang-format` and a curated `.clang-tidy`.

**D0.12 — Host sysroot discovery** (task P00.12).
- **The problem:** NixOS has no `/usr/include`. glibc headers and `crt1.o` live in the Nix store, and `/lib64/ld-linux-x86-64.so.2` is a nix-ld shim. The Clang and LLD embedded in `gopp` don't use the Nix clang wrapper, so they don't find these files by themselves.
- **Native builds:** `gopp` asks the system C compiler once (`cc -print-file-name=crt1.o`, `cc -E -v -x c /dev/null`), or reads `NIX_CFLAGS_COMPILE`/`NIX_LDFLAGS`. It records the include dirs, crt objects and dynamic linker in the build cache.
- **Fallback:** bundled static musl.
- M0 must build on NixOS and on a standard FHS distro (a Debian/Ubuntu container).

**D0.13 — A fast dev loop for `gopp` itself.** Developing the compiler means a lot of code and thousands of tests, so waiting must stay short. A slow loop is a bug, fixed like any other bug.

| Loop (on the dev machine) | Budget |
|---|---|
| Edit one `.cpp` → `gopp` rebuilt | ≤ 10 s |
| One stage's tests (`check-parser`, …) | ≤ 10 s |
| Full `ninja check` | ≤ 5 min |
| One normal test | ≤ 2 s (longer only if marked long) |
| Clean build of `gopp` (LLVM comes from Nix) | ≤ 2 min |

- **Building `gopp`** (P00.13):
  - Dev builds link LLVM and Clang as shared libraries (`libLLVM.so`, `libclang-cpp.so`) with mold. Release builds link them statically.
  - The front-end libraries (lexer, parser, type checker) may use LLVM's ADT/Support headers, but never LLVM IR or Clang headers. Only codegen and interop include those, through precompiled headers.
  - The dev build type is `-O1 -g` with assertions. `-O0` makes every test slower.
  - Build folders are never deleted. Each worktree has its own build folder, and all of them share one ccache.
- **Testing** (P00.14, P04.19):
  - Tests stop as early as they can: most check an AST or GIR dump with FileCheck and never generate code or link.
  - Each stage has its own target (`check-parser`, `check-sema`, `check-gir`, `check-codegen`, …); `check` runs them all on every core.
  - Slow tests are opt-in: cdep CMake builds, fuzzing and big benchmarks run in `check-long`, nightly, or when their own code changes.
  - Every run prints its 20 slowest tests, and CI fails when the suite gets more than 10% slower.
- **Agents** follow these rules through the repo's `CLAUDE.md`: the narrowest test first, the full suite once before finishing, and never more than 2 minutes of waiting on one command.

## Deliverables

- A git repository with the D0.5 layout, LICENSE, README, CONTRIBUTING stub, `.gitignore`, `.clang-format`, `.clang-tidy`.
- A `flake.nix` dev shell, plus the optional assertions-enabled shell (D0.4).
- Per-stage test targets, `check-long`, the slowest-tests report and the suite-time gate (D0.13).
- A CMake build that produces `gopp` linked against LLVM, Clang and LLD, with a working `gopp version`.
- A lit + FileCheck harness and a GoogleTest harness, each with a smoke test, run by `ninja check`.
- A CI pipeline.
- Host sysroot discovery (D0.12).
- ADR-0000 (template) through ADR-0005.

## Exit criteria

- Fresh clone → `nix develop` → `cmake -G Ninja … && ninja` → `gopp version` works.
- `ninja check` passes locally and in CI.
- The Debug+ASan/UBSan build of `gopp` passes CI.
- The assertions-enabled CI job passes.
- The D0.13 budgets are measured and met for the P00 build and tests.

## Risks

- **LLVM dev packaging on NixOS** (split outputs, wrapped clang) → reference the `llvmPackages_N` dev outputs for LLVM, libclang and lld explicitly in the flake, and document `LLVM_DIR`/`Clang_DIR`.
- **Static LLVM link size and time** → shared-library dev builds; static linking only for releases.
- **Building too much infrastructure before writing a compiler** → P00 is capped at size S; docs site and release tooling belong to P14/P16.

## Open questions

- **Q1** (decided 2026-10-27) Code hosting and CI: GitHub and GitHub Actions (D0.7).
- **Q2** (decided 2026-10-27) License: Apache-2.0 WITH LLVM-exception (D0.10).
- **Q3** (decided 2026-10-27) Name: the language is Go++, and everything else is `gopp` (D0.8). The trademark check stays in P16.02.
