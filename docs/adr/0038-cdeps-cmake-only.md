# ADR-0038: C and C++ dependencies are built with CMake only

- **Status:** accepted (owner decision)
- **Date:** 2026-11-11
- **Plan reference:** P11 Q1, D11.3, P11.01; related [ADR-0007](0007-build-profiles.md), [ADR-0012](0012-host-profile.md), [ADR-0037](0037-v1-targets.md), spec [FOR-10](../spec/14-foreign-surface.md)

## Context

A `cdep` in `gopp.mod` names a C or C++ library that `gopp` fetches, builds from source and links. The build is the library's own CMake project, driven by a toolchain file that carries the compiler, the target, the profile and the static or shared mode (ADR-0007).

Many libraries have no CMake project: a plain Makefile (Lua), autotools (GMP, ncurses), Meson (GLib). The design did not say what a project does with them.

## Decision

1. **A `cdep` is always a CMake project.** `gopp` supports no other build system and runs no custom build command.
2. **A library without CMake is used through a CMake project that its user writes.** That project lives in one of two places:
   - **a local directory** of the user's project, which holds the `CMakeLists.txt` and the library's sources;
   - **the user's own repository** (a fork, or a small repository that adds the `CMakeLists.txt`), named by the `cdep` like any other.
3. **`replace` works for a `cdep` path as it does for a `require` path:** it points the path at a directory or at another repository. So the import paths in source files keep the library's original path while the CMake project comes from the local directory or the fork.
4. The tree a `cdep` is built from must be complete: the build fetches nothing.
5. **A library that is already installed on the machine is not a `cdep`.** It is used with a `<…>` header import and an `ldflags` line in `gopp.mod` (FOR-10).

## Consequences

- One build path to make correct for both targets of version 1. CMake behaves the same on Linux and Windows; autotools and plain Makefiles do not, and Windows has no system packages to fall back on.
- `gopp` always controls the compiler, the target and the profile of a dependency, so cross builds, sanitizers and the cache key stay exact.
- No dependency can make `gopp` run an arbitrary command.
- **The cost:** using a library without CMake means writing and keeping a `CMakeLists.txt` for it. For a small library that is a handful of lines; for a large autotools project it is real work.
- To do: `replace` for `cdep` paths (P10, P11.01).

## Alternatives

- **More build systems** (autotools, Meson, Make): each needs its own way to pass compiler, target, profile and link mode, and each differs on Windows.
- **A custom build command in the `cdep` block:** flexible, but the flags and the target are then out of `gopp`'s hands, and a dependency could run anything.
- **A list of source files in `gopp.mod` that `gopp` compiles itself:** no external tool, fine for small libraries; not chosen now, and not needed while a local `CMakeLists.txt` does the same job.
