# ADR-0007: Build profiles

- **Status:** accepted
- **Date:** 2026-10-27
- **Plan reference:** P06 D6.12 (new), D6.3, D6.5, D6.6; P04 D4.8; P09 D9.12 (new), D9.6, D9.8, D9.9; P10 D10.2, D10.3, D10.11 (new); P11 D11.3, D11.4, D11.5, D11.7, D11.13 (new); P14 D14.1. Spec: MAC-6, MAC-8, MAC-11, MAC-15 (new), LEX-11, FOR-10.
- **Related:** ADR-0006 (D24 moved the C/C++ flags into gopp.mod). README principle 4 ("language semantics are identical").

## Context

Until now the plan had three fixed "build modes" (`debug`, `dev`, `release`) and a few loose flags (`-panic=abort`, `-race`, `-asan`). That is not enough:

- People want one more step than two modes: "release, but with ThinLTO and the native CPU", or "debug, but my dependencies at `opt 2` so the program is usable".
- Settings were spread over several places. A cdep got `CMAKE_BUILD_TYPE` from "the build mode", but nothing said how `-O` levels, debug info or LTO reached CMake.
- Output went to unclear places. The cache is global and content-addressed, but a developer also wants a visible folder with the binary.
- Code sometimes needs to know the mode (extra checks in debug only).

The owner asked for Cargo-style profiles on 2026-10-27.

## Decision

1. **Profiles.** A profile is a named set of nine settings: `opt`, `debuginfo`, `lto`, `panic`, `sanitize`, `strip`, `cpu`, `pgo`, `link`. Two exist built in: `debug` (the default; the old `dev` pipeline at `opt 0`, shared libraries for cdeps) and `release` (`opt 3`, `lto thin`, `strip true`, static cdeps). The old three build modes are removed. The plain O0 + FastISel pipeline stays only as a measuring baseline (`-pipeline=fastisel`).
2. **gopp.mod.** Custom profiles live in `profile NAME ( … )` blocks in gopp.mod with `inherits`. `deps ( … )` rows give dependencies other `opt` or `debuginfo`. Only the main module's profiles count.
3. **CLI.** `gopp build` uses `debug`; `-release` or `-profile=NAME` picks another; `-opt=2` and the other eight setting flags override once. `-race`, `-asan`, `-ubsan` and `-msan` stay as shorthands for `-sanitize`; `-panic=abort` is the `-panic=` override.
4. **Semantics.** A profile never changes what a program means. The one exception is `panic abort` (`recover()` returns nil). Sanitizers only watch for undefined behavior.
5. **CMake.** The profile reaches every cdep through gopp's toolchain file (`GOPP_PROFILE`, `CMAKE_BUILD_TYPE`, flags, LTO, `BUILD_SHARED_LIBS`). The profile is in the cdep cache key. A cdep block can pin `opt` and `debuginfo`.
6. **Compile-time access.** `#Profile()`, `#Setting("name")` and `@If(cond)` (all compile-time constants, MAC-14), plus profile names as `//gopp:build` terms. Dead code stays type-checked.
7. **Layout.** `./target/<profile>/` holds the binary, `tests/` and `cdeps/` (CMake build trees). `-target-dir` and `GOPPTARGETDIR` change it. `gopp new` adds `target/` to `.gitignore`. The content-addressed cache stays global in `~/.cache/gopp`, shared by all projects and worktrees; `target/` is never an input.

## Consequences

- One place (gopp.mod, with the CLI on top) says how a project is built. The CMake side becomes mechanical.
- Cache keys use the effective settings, so equal settings share entries across profile names, and a new custom profile mostly hits the cache.
- `debug` and `release` can sit side by side; a new worktree only links.
- `#Profile()` and `@If` let a program differ per profile. That is the author's choice, and the code stays type-checked in every profile, so debug-only code cannot rot in release. The price: `@If(false)` declarations and dead branches are still compiled by the front end.
- The `-pipeline=fastisel` baseline and the D6.9 fallback now need their own measurements outside the profile system.
- More surface to test: the nine settings times the pipeline, CMake and cache-key paths (P06.14 to P06.16, P09.13, P10.12, P11.19, P11.20, P14.11).
- A profile name must not equal a GOOS, GOARCH or build-tag word.
- Open: which settings apply to host builds of generator tools (P11 Q4).

## Alternatives

- **Flags only, as in Go.** Go has no profiles: `go build -gcflags=… -ldflags=…` and `-race`. Simple, but the flags must be typed on every command, nothing records the team's choice, and flags for the compiler do not reach C libraries. Rejected for the first two reasons; we keep Go-style flags as one-off overrides.
- **Profiles in a separate file (`Cargo.toml`-style, or a new `gopp.profiles`).** Clean separation, but gopp.mod already holds all other build facts (`cxxstd`, `cflags`, `cdep`) and is hashed and reviewed with the module. A second file means a second parser, a second place to look, and a second thing to vendor. Rejected. Profiles stay in gopp.mod, and the main-module-only rule keeps dependencies from changing your build.
- **Per-file flags (for example `//gopp:opt 3` in a source file).** Fine-grained, but the plan just removed per-file `//gopp:cflags` and friends (ADR-0006, D24) because settings hidden in files are hard to find and hash. It also breaks one-module-per-package inlining. Rejected. The finest unit is a dependency (`deps` rows) and a cdep (the pin).
- **Everything in the global cache, no `target/`.** One less folder, and Go does it too (the binary goes to the current directory, objects to the cache). But two profiles would overwrite one binary, CMake build trees would be hidden, and there would be nowhere obvious to look when a cdep build fails. Rejected. The cache stays global (sharing, hits across worktrees); `target/` holds only outputs for people.
- **A `target/` per project that also holds the compiled packages (Cargo's way).** Rejected: it would lose cross-project and cross-worktree cache hits, which the content-addressed cache (P06 D6.3) already gives, and make `target/` an input to builds.
- **Keep three modes (`debug`, `dev`, `release`) next to profiles.** Rejected: `dev` and `debug` differ only in the pipeline, which `opt` already selects.
