# P11 C/C++ deps from the internet + CMake: STATE

Legend: `[ ] not_started` · `[~] in_progress` · `[x] completed`
Phase status: `[ ] not_started`
Last updated: 2026-10-27
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

## Exit criteria

- [ ] M7: matrix builds from a clean cache on linux/amd64 and cross to linux/arm64 (QEMU); `mode dynamic`/`csystem` entries skipped on static musl
- [ ] Offline rebuild from cache and `vendor/`
- [ ] Tampering detected (tree, archive, moved tag)
- [ ] Unlocked REQUIRED `find_package`/`FetchContent` blocked with an actionable error; optional ones recorded as "not found"
- [ ] Cross-project cache hit without running CMake

## Decision changes

- None.

## Notes / blockers

- System CMake 4.1.2 works through `GOPPCMAKE` only if it is ≥ the pinned minimum (P11.04).
