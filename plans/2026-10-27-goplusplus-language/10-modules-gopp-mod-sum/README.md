# P10 — Modules: gopp.mod and gopp.sum

**Goal:** Go-modules-grade dependency management for Go++ code. The same file formats also lock C/C++ dependencies; how those `cdep` sources are built is P11's job.
**Depends on:** P06 · **Unblocks:** P11 · **Size:** M · **Milestone:** M7 (with P11)

## Decisions

**D10.1 — Adopt Go modules' model wholesale for Go++ packages.**
- The module path is the import-path prefix.
- Versions are semver tags `vX.Y.Z`. Untagged commits get pseudo-versions.
- Semantic import versioning (`/v2`).
- Version selection uses Minimal Version Selection.
- `replace`, `exclude`, `retract`, `internal/` and vendoring work as in Go.
- **Classification:** the longest prefix over all `require` and `cdep` paths decides what an import is. A match on the main module or a `require` is a Go++ package, unless the path ends in a header extension (P01 D1.15): then it names a header file inside that module. A `cdep` match is a header import (P11 D11.4). The same path in both is an error.

*Why:* this model is proven, deterministic, fast, needs no SAT solver, and is already familiar to Go developers.

**D10.2 — gopp.mod grammar = go.mod grammar plus these directives.**
- `gopp <language version>` and `toolchain gopp<version>`.
- `cdep <path> <version> ( … )`: one block per C/C++ package.
  - The path is Go-style (`github.com/madler/zlib`) and is also the prefix of the package's header imports.
  - Keys: `commit`, `url`, `tag`, `mode`, `cmake`, `provides`, `uses`, `option`, `patch`. `tag` is defined in D10.4; the others in P11.
  - The `cdep` blocks of transitive dependencies carry `// indirect` (D10.3).
- `cxxstdlib libc++|libstdc++`.
- `csystem <name> ( pkgconfig "<expr>" … )`: non-hermetic system libraries, recorded but never hashed. Each entry also names the headers it owns (P11 D11.11).

```
module github.com/acme/imgtool

gopp 0.1

require (
    github.com/acme/geom v1.4.2
    github.com/acme/log  v0.3.0 // indirect
)

cdep github.com/madler/zlib v1.3.1 (
    commit   <full commit hash that tag v1.3.1 resolved to>
    mode     static
    provides ZLIB
    option   ZLIB_BUILD_EXAMPLES=OFF
)

cdep sqlite.org/sqlite v3.49.1 (
    url      https://www.sqlite.org/2025/sqlite-amalgamation-3490100.zip
    cmake    ./cmake/sqlite.cmake
)
```

Go++ code imports these as `import "github.com/madler/zlib/zlib.h"` and `import "sqlite.org/sqlite/sqlite3.h"`.

(Illustrative only: the URL and versions are examples, not verified pins.)

**D10.3 — Lock in *both* files (R10).**
- **gopp.mod pins identity.**
  - Every requirement has an exact version.
  - VCS cdeps also pin the full commit hash, because tags can be moved and commits can't.
  - Archive cdeps also pin the exact `url`.
- **gopp.sum pins content.** Each kind of line has its own form, so cdep lines are distinct from Go++ module lines:
  - `path version h1:…` and `path version/gopp.mod h1:…` for every Go++ module and its gopp.mod (Go's SHA-256 dirhash);
  - `path version/cdep h1:…` for every cdep tree (tree format: D10.10);
  - `path version/archive sha256:…` for every archive cdep: the hash of the raw archive, verified *before* extraction;
  - `path version/notfound Name` for an optional `find_package(Name)` request that the cdep's configure answered "not found" (P11 D11.3), so every machine gives the same answer.
- **Listing.** The main gopp.mod lists every selected cdep. Transitive ones carry `// indirect`, and `gopp mod tidy` writes them.
- **Rules across modules.** Several modules may declare the same cdep.
  - **Identity keys** (`commit`, `url`) must be equal everywhere for a version. Otherwise it is an error that names the modules.
  - **Build keys** (`mode`, `option`, `patch`, `cmake`, `provides`, `uses`): the main module's own block (without `// indirect`) wins. An `// indirect` block is a copy that `tidy` refreshes from the declaring modules, which must agree. If they don't, `tidy` reports an error, and the main module settles it by writing its own block.
  - **Main-module only:** `cxxstdlib`, `csystem` and `toolchain` count only in the main module.
  - **Files:** patch and local CMake files are read from the module that declares them. `gopp mod vendor` copies them.
- **Verification** follows P06 D6.10.
  - Hashes are checked when downloading and extracting into the read-only cache. A mismatch is a hard "SECURITY ERROR", as in Go.
  - Each build only checks that gopp.sum has every entry and that the cached trees are marked verified. A missing entry is an error that suggests `gopp mod tidy` or `gopp get`.
  - `gopp mod verify` re-hashes the cache on demand.
  - Tags are checked only by `gopp get` and `gopp mod tidy`. A tag that now points to a different commit than gopp.mod pins is an error.

```
github.com/acme/geom v1.4.2 h1:<base64>=
github.com/acme/geom v1.4.2/gopp.mod h1:<base64>=
github.com/acme/log v0.3.0 h1:<base64>=
github.com/acme/log v0.3.0/gopp.mod h1:<base64>=
github.com/madler/zlib v1.3.1/cdep h1:<base64>=
sqlite.org/sqlite v3.49.1/archive sha256:<hex>
sqlite.org/sqlite v3.49.1/cdep h1:<base64>=
```

A dependency's own cdep blocks live in its gopp.mod, and its `/gopp.mod h1:` line covers that file. So a change to a dependency's build keys changes a hash you can review.

**D10.4 — Fetching.**
- Sources: direct VCS (git) and HTTPS archives.
- `GOPPPROXY` speaks the same HTTP protocol shape as GOPROXY (`/@v/list`, `.info`, `.mod`, `.zip`), so a static file server or an Athens-like proxy works. cdep trees are served as tar archives (D10.10). The default is `GOPPPROXY=direct` until a public proxy exists.
- `GOPPPRIVATE`, `GOPPINSECURE` and `GOPPNOPROXY` work like their Go equivalents.
- Authentication uses git credentials or `.netrc`.
- **cdep versions.** Many C/C++ projects don't tag like Go (`vX.Y.Z`).
  - A `tag` key, or a pattern, maps a version to the repository's tag: fmt `12.2.0`, `openssl-3.3.1`, `curl-8_9_1`.
  - Repositories without tags use pseudo-versions.
  - When requirers need different major versions of a cdep, the main module must pin one explicitly. There is no silent major upgrade.

**D10.5 — Module cache.**
- `$GOPPMODCACHE` (default `~/.local/share/gopp/mod`, the XDG data dir) holds read-only extracted trees plus the downloaded zips and archives.
- It is separate from the build cache (P06 D6.3), as in Go.
  - gopp never trims it.
  - `gopp clean -modcache` deletes it.
- cdep sources live in the same cache.
- Concurrent `gopp` processes coordinate through file locks.

**D10.6 — Fetching never executes code.**
- This is Go's guarantee.
- The one exception is `gopp get -c`: its discovery configure runs the package's CMake code inside the sandbox (P11 D11.3).
- Building cdeps is a separate, sandboxed step (P11).

**D10.7 — Commands.**
- `gopp mod init|tidy|download|verify|graph|why|edit|vendor`, `gopp get path@version`, `gopp list -m all`.
- `tidy` also writes the `// indirect` cdep blocks (D10.3), and adds and prunes cdep sum lines.
- `verify` re-hashes the cache against gopp.sum.

**D10.8 — Checksum database / transparency log after v1.**
- It needs infrastructure, as sum.golang.org does. gopp.sum is designed so one can be added later (`GOPPSUMDB`).
- Until then, first fetches are trust-on-first-use. This is documented, and vendoring is recommended for high-assurance builds.

**D10.9 — Workspaces (`gopp.work`) after v1,** unless monorepo development needs them earlier.

**D10.10 — cdep tree format and hash.**
- The tree hash keeps in-tree symlinks and the exec bit.
- Submodules are fetched at their pinned commits and included in the tree.
- There is no `vendor/` filtering, and the size limit is larger than Go's 500 MiB.
- Proxies serve tar archives, which keep file modes.
- The same path@version with different `url`s is an error.
- The hash goes in the `path version/cdep h1:` line of gopp.sum (D10.3).

*Why:* Go's module zip drops symlinks and exec bits, skips submodules, filters `vendor/` and stops at 500 MiB. Many C/C++ source trees need all of these.

## Deliverables

- `compiler/lib/Mod`:
  - gopp.mod/gopp.sum parser and printer;
  - version logic, including the cdep version → tag mapping;
  - MVS and the cdep rules across modules;
  - fetchers (with submodules), cache;
  - the zip/dirhash implementation and the cdep tree format.
- The commands above.
- Tests: ported Go MVS cases, tamper tests, offline tests, canonical formatting, cdep conflicts across modules, cdep trees with symlinks, exec bits and submodules.
- `docs/modules.md`.

## Exit criteria

- A multi-module sample resolves and builds.
- `gopp mod tidy` is idempotent.
- cdep blocks follow the rules across modules (D10.3): `tidy` writes the `// indirect` blocks, and conflicting identity keys are an error that names the modules.
- Tampering is detected where P06 D6.10 checks it: a changed archive when downloading, an edited cache file by `gopp mod verify`, a moved tag by `gopp get` and `gopp mod tidy`.
- Offline builds work from the cache and from `vendor/`.
- `gopp mod edit -fmt` output is canonical.
- A cdep tree with symlinks, exec bits and a submodule gets the same `/cdep h1:` hash from git and from a proxy (D10.10).

## Risks

- **Trust-on-first-use without a checksum DB** → documented; vendoring; a sumdb after v1.
- **Divergence from go.mod tooling expectations** → keep the grammar a strict superset of go.mod's.

## Open questions

- **Q1** (decided 2026-10-27) Default cache dirs on Linux, the only v0.1 platform: the module cache is `~/.local/share/gopp/mod` (D10.5). macOS and Windows get theirs when those platforms are added (P16).
- **Q2** Should `gopp` ever consume Go modules? Proposed: no, since it is a different language; a conversion tool might come later.
