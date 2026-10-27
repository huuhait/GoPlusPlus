# P11 — C/C++ dependencies from the internet, and CMake

**Goal:** depend on any C/C++ project fetched from the internet, and import its headers with Go-style paths: `import "github.com/madler/zlib/zlib.h"`. Each dependency is locked in gopp.mod + gopp.sum, built hermetically with gopp's own toolchain, cached, and cross-compiled. CMake is the only build system in v1; a package without CMake gets a small local CMake file.
**Depends on:** P07, P08 (for C++), P10 · **Size:** L · **Milestone:** M7

## Decisions

**D11.1 — Every cdep has a Go-style path.** The path names the package in gopp.mod and is the prefix of its header imports.

| Kind | Identity (gopp.mod) | Content (gopp.sum) | Notes |
|---|---|---|---|
| Git repository (`github.com/madler/zlib`) | path + version + full commit (+ `tag`, P10 D10.4) | `/cdep h1:` of the tree | default; the path says where to fetch from |
| Archive (`sqlite.org/sqlite` + `url`) | path + version + exact `url` | `/archive sha256:` of the archive + `/cdep h1:` of the tree | archive verified before extraction; a single top-level directory is stripped |
| Local path | `replace … => ../dir` | — | development only; never locked |
| System | `csystem` + pkg-config module + the headers it owns (D11.11) | — (recorded, not hashed) | rejected under `-hermetic` |

One path@version with different `url`s is an error (P10 D10.10).

**D11.2 — CMake only (v1).**
- Every cdep is built with CMake.
- A package without a `CMakeLists.txt` (Dear ImGui, the sqlite amalgamation, stb, …) gets a local CMake file in your module, named by the `cmake` key. You write it yourself:

  ```
  cdep github.com/ocornut/imgui v1.91.5 (
      commit <full commit hash that tag v1.91.5 resolved to>
      cmake  ./cmake/DearImgui.cmake
  )
  ```

  ```cmake
  # ./cmake/DearImgui.cmake. GOPP_CDEP_SOURCE_DIR is the fetched, hash-verified imgui tree.
  add_library(imgui
      ${GOPP_CDEP_SOURCE_DIR}/imgui.cpp
      ${GOPP_CDEP_SOURCE_DIR}/imgui_draw.cpp
      ${GOPP_CDEP_SOURCE_DIR}/imgui_tables.cpp
      ${GOPP_CDEP_SOURCE_DIR}/imgui_widgets.cpp)
  target_include_directories(imgui PUBLIC ${GOPP_CDEP_SOURCE_DIR})
  ```

  Go++ code then writes `import "github.com/ocornut/imgui/imgui.h"` and calls `imgui.Begin("Hello")`.
- gopp runs the file through a tiny generated top-level project that sets `GOPP_CDEP_SOURCE_DIR` and includes it.
- The same key is also an escape hatch: it replaces a package's own CMake build when that build misbehaves.
- Local CMake files and patches are read from the module that declares them (P10 D10.3). So a local file is versioned with that module's code, and covered by its `h1:` when that module is someone else's dependency.
- The local file's hash, and the hashes of the files it `include()`s, are part of the cdep's build cache key (D11.5).
- Other build systems (Meson, Autotools, …) may come after v1 (Q1).

*(Versions in examples are illustrative, not verified pins.)*

**D11.3 — CMake bridge.**
- **Pinned CMake + Ninja.** The gopp toolchain pins exact versions, downloads them on first use, verifies their hashes and caches them. The pinned CMake version is also the minimum: `GOPPCMAKE` may point to a system CMake instead, only if it is ≥ the pinned version.
- **Generated toolchain file:**
  - `CMAKE_C_COMPILER`/`CMAKE_CXX_COMPILER` set to `gopp cc`/`gopp c++`;
  - target triple and sysroot;
  - the build profile: `GOPP_PROFILE`, `CMAKE_BUILD_TYPE`, the optimization, debug-info, CPU, PGO and sanitizer flags, LTO and `BUILD_SHARED_LIBS` (D11.13);
  - position-independent code on;
  - `-ffile-prefix-map` for reproducibility;
  - the C++ stdlib from gopp.mod.
- **Dependency provider.** Uses `cmake_language(SET_DEPENDENCY_PROVIDER …)` (CMake 3.24+), injected through `CMAKE_PROJECT_TOP_LEVEL_INCLUDES`. It intercepts every `find_package()` and `FetchContent_MakeAvailable()`.
  - **System packages:** a built-in list (Threads, …) that CMake's own modules resolve against the gopp sysroot.
  - **`csystem` packages:** `find_package` names can map to `csystem` entries (X11, OpenGL, Wayland, ALSA; D11.11).
  - **Locked packages** are other cdeps in the lock, matched by their `provides` names. For example, libpng's `find_package(ZLIB)` finds the locked zlib. A request for one is satisfied in this order:
    1. the cdep's own installed config;
    2. CMake's Find module, with `<Name>_ROOT` set to the cdep's prefix;
    3. a generated config (last resort; see below).
  - **Unlocked packages:**
    - an *optional* request answers "not found", and that answer is recorded in gopp.sum as a `/notfound` line (P10 D10.3);
    - only a REQUIRED request fails configure, with: `unlocked dependency "PNG": add it with gopp get -c <path>@<version>`.
  - `FETCHCONTENT_FULLY_DISCONNECTED=ON` is set as a second guard.
- **Host isolation.** A cdep build can't see host libraries:
  - a curated sysroot with only libc and kernel headers;
  - `CMAKE_FIND_ROOT_PATH`, with `CMAKE_FIND_ROOT_PATH_MODE_{LIBRARY,INCLUDE,PACKAGE}=ONLY`;
  - `PKG_CONFIG_LIBDIR` limited to gopp's `.pc` dirs;
  - a clean environment.
- **Host tools** (nasm, perl, python, …) are either shipped with gopp, or their versions are recorded in the cache key (D11.5).
- **Discovery mode** (`gopp get -c`, D11.9) records every request and never fails.
- **Sandbox.** Configure and build run with no network, on a private copy of the source tree, because some CMake projects write into their own source directory. The cached tree stays pristine.
  - Landlock read/write allow-lists (Linux ≥ 5.13, no privileges needed), plus mount and network namespaces when available.
  - No network even without namespaces: a seccomp filter (no privileges needed) denies `socket()` for IP address families and `io_uring_setup`.
  - Read-only: the toolchain, the sysroot, cdep sources and dependency prefixes.
  - Read-write: the private source copy, the build dir and a staging install dir. gopp copies the staging dir into the cache.
  - Ubuntu 24.04 restricts unprivileged user namespaces, so Landlock is the base layer.
  - Without Landlock (older kernels): best effort, with a loud warning. `-hermetic` refuses to build (D11.11).
  - This is what catches `ExternalProject_Add`, `file(DOWNLOAD)` and stray network calls from `execute_process`.
- **Build and install.** Build with Ninja, then `cmake --install` into the staging dir (`DESTDIR`), under a fixed virtual prefix (D11.5). gopp then copies the staged files into the cdep's prefix inside the cache.
- **Probe project.** For each set of imports, gopp generates a tiny consumer project that links the chosen targets. Through the CMake File API, gopp reads the probe's `compileGroups` (the full transitive usage flags) and its `link.commandFragments` (the full, ordered link line). Both exist since codemodel 2.0.
- **Generated configs** (last resort). For a locked cdep with no installed config and no CMake Find module, gopp writes a `<Provides>Config.cmake` into its prefix. If the cdep installs no pkg-config `.pc` file, gopp writes one too, so downstream `pkg_check_modules` calls also resolve to gopp-built artifacts.

**D11.4 — Imports, `mode static` or `mode dynamic` (default: from the profile's `link`, D11.13), and the link line.**
- **Full-path imports.** A header is imported by package path plus the header's path inside the package, exactly like a Go import path:
  - `import "github.com/madler/zlib/zlib.h"`
  - `import "github.com/fmtlib/fmt/include/fmt/format.h"`
  - `import "sqlite.org/sqlite/sqlite3.h"`

  An import whose longest matching prefix is a cdep's path is a header import (P10 D10.1). It points at exactly one file; nothing is shortened or searched. The header becomes a package named after the file, used like a Go package: `zlib.crc32(…)` (P07 D7.3, P08 D8.1).
- **Source paths, installed copies.** The path after the cdep's path is the header's path in the cdep's source tree, as shown on GitHub. gopp records a source → installed map at build time and parses the installed copy.
  - A generated header (such as OpenSSL's `ssl.h`) is named by its path in the build tree.
  - Headers are never parsed from the pristine cache.
- **Static or dynamic.** An explicit `mode` in the cdep block is **strict**: if the library cannot be built that way, it is an error. Without `mode`, the profile's `link` setting (D11.13; `dynamic` in `debug`, `static` in `release`) is only a **preference**: gopp uses that kind if the cdep builds it, and otherwise the kind it does build, with no error. On static-musl targets it is always static. Example: a library that builds only a static archive, in a `debug` build (`link dynamic`), links static.
  - `static`: configure with `BUILD_SHARED_LIBS=OFF` and link static archives into the binary, giving one self-contained binary like Go.
  - `dynamic`: configure with `BUILD_SHARED_LIBS=ON` and link shared libraries. Use this for packages that only build shared libraries.
    - Build with `CMAKE_INSTALL_RPATH=$ORIGIN`.
    - gopp copies the libraries next to the output binary, under their SONAME.
    - If a cdep is `dynamic`, every cdep it `uses` (D11.10) must be `dynamic` too. The kinds are settled in this order:
      1. Cdeps with an explicit `mode` are fixed first.
      2. A cdep without `mode` that an explicit `mode dynamic` cdep `uses` becomes `dynamic` if it can build a shared library. If it can't, it is an error that names both cdeps.
      3. All other cdeps without `mode` follow the profile's `link` preference (with the fallback above), settled from the leaves up: if a cdep it `uses` ends up static, this one falls back to static.
      4. The only direct mismatch error left is an explicit `mode static` cdep used by an explicit `mode dynamic` cdep.
    - Example: `cdep A` has `mode dynamic` and `uses B`; `cdep B` has no `mode`. In a `release` build (`link static`), B is still `dynamic` by rule 2, so the chain builds in both profiles.
    - Unsupported on static-musl targets (D11.12).
- **Target choice.** One fixed rule picks the library target to link:
  - keep only the non-INTERFACE library targets that match `mode` (STATIC for `static`, SHARED for `dynamic`);
  - among them, take the target named after the package, or else the only one left;
  - if the choice is still ambiguous, it is an error that lists the candidates. A local CMake file can narrow the choice (D11.2).
  - If no target matches an explicit `mode`, the error says why, e.g. "github.com/x/y builds no static library; use `mode dynamic`".
  - Example: zlib 1.3.1 builds `zlibstatic` and `zlib`, so `mode static` links `zlibstatic` and `mode dynamic` links `zlib`.
- **Flags and link line** come from the probe project (D11.3): the full transitive usage flags, and the full, ordered link line.
- **Language.** The header is parsed with the probe's include dirs and defines. It is parsed as C++ if the target compiles C++ or the header has a C++ extension (P01 D1.15), otherwise as C.

**D11.5 — Cache keys and reproducibility.**
- A cdep build is keyed by the SHA-256 of:
  - the source tree hash (`/cdep h1:`, P10 D10.10);
  - the normalized cdep block (`mode`, options, patches), plus the hash of its local CMake file, if any, and of every file that file `include()`s;
  - the toolchain identity (gopp + LLVM versions), plus the CMake and Ninja versions;
  - a sysroot fingerprint;
  - the scrubbed environment, which is also recorded;
  - host tool versions (D11.3);
  - target triple, C++ stdlib, the profile name (CMake sees it as `GOPP_PROFILE`) and the cdep's effective profile settings after its pin, sanitizers included (D11.13);
  - the `uses` edges (D11.10) and the build keys of the cdeps they point to.
- Installs go through `DESTDIR` staging into a fixed virtual prefix, so outputs don't depend on where the cache lives.
- Builds are shared across all projects on the machine. The profile name is in the key, so `debug` and `release` builds of a cdep are different entries.
- Outputs are deterministic: `SOURCE_DATE_EPOCH`, deterministic archives, prefix maps.

**D11.6 — One version per C/C++ library per binary.**
- C and C++ share one global symbol namespace, and the ODR forbids duplicate definitions. Unlike Go++ modules (`/v2`), two versions of a C/C++ library can't coexist.
- MVS applies when both versions are semver-comparable and have the same major version.
- Otherwise the main module must pin a version (P10 D10.4). The error lists which module requires which version.
- **Bundled copies.** Some packages bundle their own copy of a library (in `third_party/` or a submodule), which bypasses this rule.
  - Before linking, gopp scans for duplicate global symbol definitions (with an allow-list) and names both providers.
  - `gopp get -c` detects bundled copies and suggests disabling them.

**D11.7 — Build flags propagate.**
- The `sanitize` setting (P06 D6.12; the flags `-asan`, `-msan`, `-race` (TSan) and `-ubsan` are shorthands) rebuilds cdeps with the same instrumentation, under separate cache keys (D11.13).
- `-msan` requires every C/C++ input to be instrumented.
- A cdep takes its compile and link flags from its own CMake target. The module-wide `cxxstd`, `cflags` and `ldflags` directives of gopp.mod (P10, P01 D1.15) apply to the Go++ module's own C/C++ sources and header imports, not to cdeps.

**D11.8 — Patches are locked too.** A `patch <file>` key applies a patch file from the module that declares the cdep (P10 D10.3), for small source fixes. The patch's hash is part of the cache key. To replace a package's build entirely, use a local CMake file (D11.2).

**D11.9 — Commands.**
- `gopp get -c <path>@<version>`:
  - detects the version → tag mapping (P10 D10.4), resolves the tag to a commit, downloads and hashes the source;
  - runs a discovery configure inside the sandbox (D11.3). Discovery never fails. It records every request, and lists the package's libraries and options;
  - suggests cdeps and `csystem` mappings for those requests;
  - detects bundled copies (`third_party/`, submodules) and suggests disabling them (D11.6);
  - writes the cdep block, with its `uses` edges (D11.10), and the sum lines.

  If the package has no `CMakeLists.txt`, it stops and asks for a local CMake file (D11.2).
- `gopp mod tidy`: verifies and prunes cdep entries, and records `uses` edges (D11.10).
- `gopp cdep targets <dep>`: lists the dependency's CMake targets and the headers each one exposes.
- `gopp cdep build <dep> -v`: builds one cdep with verbose output, for debugging.
- `gopp mod vendor`: includes cdep sources, plus the patch and local CMake files they need (P10 D10.3).

**D11.10 — Transitive cdeps.**
- A cdep's `uses` key lists the cdeps it needs, by their `provides` names. Example: libpng `uses ZLIB`.
- `gopp get -c` and `gopp mod tidy` record these edges in gopp.mod. They are checked at configure time.
- Build order and cache keys come from these edges (D11.5), so a cache hit needs no CMake run.

**D11.11 — System libraries (`csystem`) and `-hermetic`.**
- A `csystem` entry names its pkg-config module and the headers it owns. So a `"<…>"` import of one of those headers links the right library.
- `find_package` names map to `csystem` entries (D11.3).
- `-hermetic` rejects `csystem` entries. It also refuses to build without Landlock (D11.3).

**D11.12 — Cross builds.**
- Generator tools that run during the build (protoc, tblgen) get a host build of their cdep.
- `CMAKE_CROSSCOMPILING_EMULATOR` (QEMU user mode) runs `try_run` checks.
- `mode dynamic` and `csystem` are unsupported on static-musl targets.

**D11.13 — Build profiles reach every cdep.** (ADR-0007; tasks P11.19, P11.20.) The settings and defaults are in P06 D6.12. gopp's generated toolchain file (D11.3) turns the cdep's **effective settings** (the profile, then the `deps` row, then the cdep's own pin, then command-line overrides) into CMake variables and flags. A cdep CMake project needs no change.

| Profile setting | What the toolchain file does |
|---|---|
| (the profile's name) | sets `GOPP_PROFILE` (for example `debug`, `release`, `fast`). CMake files may read it; it is part of the cache key |
| `opt`, `debuginfo` | sets `CMAKE_BUILD_TYPE` by the table below, and puts the exact flags into `CMAKE_<LANG>_FLAGS_<CONFIG>_INIT`, so CMake's own defaults (`-O3`, `-g`) do not fight them |
| `opt` | `-O0` `-O1` `-O2` `-O3` `-Os` `-Oz` |
| `debuginfo` | `none`: no flag; `line`: `-gline-tables-only`; `full`: `-g` |
| `cpu` | `baseline`: no flag; `native` or a name: the matching `-march=` / `-mcpu=` flag (`native` is first turned into the real CPU name) |
| `pgo` | `generate`: `-fprofile-instr-generate`; a file: `-fprofile-instr-use=<file>` |
| `sanitize` | `-fsanitize=…` plus `-fno-omit-frame-pointer` for the same sanitizers as the Go++ code (P05 D5.9, D11.7), also at link time |
| `lto` | `thin` or `full`: `CMAKE_INTERPROCEDURAL_OPTIMIZATION=ON` with `-flto=thin` or `-flto=full`, so static cdeps hold bitcode that LLD can optimize together with the Go++ code. `off`: `OFF` |
| `link` | the **preferred** library kind: `BUILD_SHARED_LIBS=ON` for `dynamic` (the `debug` default), `OFF` for `static` (the `release` default). It is a preference only: if the cdep builds just the other kind, gopp uses that kind with no error, and on static-musl targets it always uses static (D11.4). An explicit `mode` is strict and overrides it |
| `strip` | `true` (= `symbols`: removes the symbol table and debug info) applies to the final binary and to shared libraries copied next to it (`cmake --install --strip`); static archives are not stripped |
| `panic` | no CMake effect. C++ exceptions **stay on** in every profile, because cdeps may throw (`-fno-exceptions` is never added). Under `panic abort`, an exception that reaches Go++ frames ends the process (P04 D4.6) |

`CMAKE_BUILD_TYPE` by the cdep's effective `opt` and `debuginfo`:

| `opt` | `debuginfo` | `CMAKE_BUILD_TYPE` |
|---|---|---|
| `0` | any | `Debug` |
| `1` `2` `3` | `none` | `Release` |
| `1` `2` `3` | `line` or `full` | `RelWithDebInfo` |
| `s` `z` | any | `MinSizeRel` |

`-DNDEBUG` is added for every type except `Debug`, as CMake does.

- **Per-cdep pin.** A cdep block can fix settings for that cdep in every profile:

  ```
  cdep github.com/madler/zlib v1.3.1 (
      commit <full commit hash that tag v1.3.1 resolved to>
      profile (
          opt 2
      )
  )
  ```

  Only `opt` and `debuginfo` can be pinned (P10 D10.11). The pin beats a `deps` row for this cdep, but a command-line override (`-opt=0`) beats the pin (P06 D6.12). The pin is a build key (P10 D10.3).
- **Cache key.** The profile name and the cdep's effective settings are part of its cache key (D11.5). The same cdep at the same settings is built again for each different profile name, because `GOPP_PROFILE` is visible to its CMake code.
- **Where it builds.** Each profile builds its cdeps in `target/<profile>/cdeps/<name>-<version>/` (P06 D6.12). That folder is only the CMake build tree, used for debugging and `gopp cdep build -v`. The installed result goes to the global cache (D11.5), so a second project or worktree still gets a cache hit with no CMake run. Build trees must not change outputs: the paths are mapped away with `-ffile-prefix-map` (D11.3).
- **Preference vs `mode`.** Only `mode` is strict. The profile's `link` and a `-link=` flag set the preference. A cdep that wants a fixed kind uses `mode`. The resolution order for `uses` chains is in D11.4: a cdep without `mode` that an explicit `mode dynamic` cdep uses becomes dynamic (error if it can't); only an explicit `mode static` under an explicit `mode dynamic` is a direct mismatch error.

## End-to-end example

```
$ gopp get -c github.com/madler/zlib@v1.3.1
  resolved v1.3.1 → <commit>; tree h1:<…>
  libraries: zlibstatic (static), zlib (shared) → link kind follows the profile (default: static in release)
  wrote cdep block to gopp.mod and 1 line to gopp.sum
$ gopp build   # import "github.com/madler/zlib/zlib.h" → sandboxed configure/build (first time) → cached libz.a → linked
$ gopp build   # cache hit; CMake does not run
```

## Deliverables

- `compiler/lib/CDep`: cdep blocks and `uses` edges, local CMake wrapper, Landlock sandbox, CMake bridge, provider script, probe project and File API reader, config/pkg-config generators, `csystem`, the duplicate-symbol scan, host builds of generator tools.
- The commands above.
- A test matrix:
  - zlib (CMake, `mode static`);
  - zlib (CMake, `mode dynamic`: shared library copied next to the binary);
  - libpng (CMake; needs zlib through `find_package`, served by the provider);
  - fmt (CMake, C++);
  - GLFW (system packages: Threads, plus X11/Wayland through `csystem`);
  - Dear ImGui (no CMake: local `./cmake/DearImgui.cmake`);
  - sqlite amalgamation (archive + local CMake file);
  - a project that uses FetchContent (must be intercepted);
  - a malicious fixture that tries network access, writes outside its sandbox and reads `~/.ssh` (all must be blocked).

## Exit criteria

- M7: the matrix builds from a clean cache on linux/amd64, and cross-compiles to linux/arm64 where it runs under QEMU. Entries that need `mode dynamic` or `csystem` are skipped on static-musl targets (D11.12).
- Offline rebuilds work from the cache and from `vendor/`.
- Tampering is detected: tree, archive, moved tag.
- An unlocked REQUIRED `find_package`, or an unlocked `FetchContent` request, is blocked with an actionable error. An unlocked optional request answers "not found", and gopp.sum records it.
- A second project using the same cdep version and profile settings gets a cache hit, with no CMake run.
- Profiles: zlib built under `debug` and `release` gives two cache entries and two build trees under `target/`; a `profile ( opt 2 )` pin takes effect in `debug`; `lto thin` produces bitcode archives that link; `link dynamic` flips `BUILD_SHARED_LIBS` unless `mode` is set.

## Risks

- **CMake projects with hard-coded compilers or flags** → `option` overrides, locked `patch` files, or replacing the package's build with a local CMake file.
- **Sandbox portability** → Landlock works on Linux ≥ 5.13 without privileges. Ubuntu 24.04 restricts unprivileged user namespaces, so namespaces are only an extra layer, used when available. Without Landlock, gopp builds with a loud warning, and `-hermetic` refuses to build (D11.11).
- **Build time of large C++ dependencies** → the shared cache. Binary caches are *not* in v1 because they are a supply-chain risk; a signed binary cache could come later (Q2).

## Open questions

- **Q1** Build systems besides CMake (Meson, Autotools), after v1.
- **Q2** Signed binary caches.
- **Q3** A Windows sandbox.
- **Q4** Which profile settings apply to host builds of generator tools (D11.12)? Proposed: the same `opt` and `debuginfo`, never `sanitize`, `lto` or `pgo`.
