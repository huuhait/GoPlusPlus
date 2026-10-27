# P11 C/C++ deps from the internet + CMake: STATE

Legend: `[ ] not_started` · `[~] in_progress` · `[x] completed`
Phase status: `[ ] not_started`
Last updated: 2026-10-27 (build profiles added)
Task definitions: [../TODO.md](../TODO.md) (section P11) · Design: [README.md](README.md)

## Tasks

- [ ] P11.01 cdep schema
- [ ] P11.02 Fetch & lock
- [ ] P11.03 Local CMake files
- [ ] P11.04 CMake & Ninja provisioning
- [ ] P11.05 Toolchain file
- [ ] P11.06 Dependency provider
- [ ] P11.07 Sandbox
- [ ] P11.08 Probe project & File API
- [ ] P11.09 Import → targets & link set
- [ ] P11.10 Cache keys & reuse
- [ ] P11.11 Version uniqueness
- [ ] P11.12 Cross-compilation
- [ ] P11.13 Commands
- [ ] P11.14 M7 matrix in CI
- [ ] P11.15 `mode dynamic` runtime
- [ ] P11.16 `csystem`
- [ ] P11.17 `-hermetic`
- [ ] P11.18 Transitive cdeps
- [ ] P11.19 Profile in the toolchain file
- [ ] P11.20 Profile in cdep keys, pins and build trees

## Test matrix

- [ ] zlib (CMake, `mode static`)
- [ ] zlib (CMake, `mode dynamic`: shared library next to the binary)
- [ ] libpng (CMake, find_package(ZLIB) via provider)
- [ ] fmt (CMake, C++)
- [ ] GLFW (system packages: Threads, plus X11/Wayland through `csystem`)
- [ ] Dear ImGui (no CMake: local `./cmake/DearImgui.cmake`)
- [ ] sqlite amalgamation (archive + local CMake file)
- [ ] FetchContent-using project (intercepted)
- [ ] Malicious fixture (network, out-of-tree writes and `~/.ssh` reads blocked)

## Open questions

- [ ] Q1 Build systems besides CMake (Meson, Autotools), after v1
- [ ] Q2 Signed binary caches
- [ ] Q3 Windows sandbox
- [ ] Q4 Profile settings for host builds of generator tools

## Exit criteria

- [ ] M7: matrix builds from a clean cache on linux/amd64 and cross to linux/arm64 (QEMU); `mode dynamic`/`csystem` entries skipped on static musl
- [ ] Offline rebuild from cache and `vendor/`
- [ ] Tampering detected (tree, archive, moved tag)
- [ ] Unlocked REQUIRED `find_package`/`FetchContent` blocked with an actionable error; optional ones recorded as "not found"
- [ ] Cross-project cache hit without running CMake
- [ ] Profiles: `debug`/`release` zlib are two cache entries; a `profile ( opt 2 )` pin works; `lto thin` links

## Decision changes

- 2026-10-27 (ADR-0006, P01.15 owner decisions): cdeps keep taking their flags from their own CMake target; the gopp.mod `cxxstd`/`cflags`/`ldflags` do not apply to them.
- 2026-10-27 (ADR-0007, build profiles): new D11.13. The toolchain file maps the build profile to CMake (replacing "CMAKE_BUILD_TYPE from the build mode"); `mode` defaults from the profile's `link`; the profile name and effective settings are in the cdep cache key; cdep build trees move to `target/<profile>/cdeps/`; cdeps may pin `opt`/`debuginfo`.

## Notes / blockers

- System CMake 4.1.2 works through `GOPPCMAKE` only if it is ≥ the pinned minimum (P11.04).
