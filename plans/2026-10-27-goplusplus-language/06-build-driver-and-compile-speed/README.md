# P06 — Build driver and compile speed

**Goal:** `gopp build/run/test` for multi-package programs at Go-like speed: export data instead of headers, a parallel and pipelined package graph, content-addressed caching, and a cheap debug pipeline. Compile-speed budgets are measured and enforced in CI.
**Depends on:** P04, P05 · **Unblocks:** P07, P09, P10, P11 · **Size:** L (budgets stay enforced until v1) · **Milestone:** M3

## Where compile time goes, and the answer

| Cost | Go's answer | Go++'s answer |
|---|---|---|
| Re-reading dependencies | export data | export data (`.gopx`), lazily decoded |
| C/C++ headers | — (cgo is slow) | import units cached as PCH + binding export data (P07) |
| Generic code | GC-shape stenciling | monomorphization + instance cache + COMDAT folding |
| Backend | its own fast backend | LLVM: a tuned small pipeline at `opt 0` (the default `debug` profile), full pipelines at higher `opt`, parallel codegen |
| Rebuilds | action cache keyed on export data | same, but finer: dependents rebuild only when the API, or a body they used, changes (D6.2) |
| Linking | its own linker | bundled LLD (multithreaded) |
| Macros | — | sandboxed interpreter + expansion cache (P09) |
| Bloated graphs | unused imports are errors | same |

## Decisions

**D6.1 — Package-level compilation, pipelined.**
- Per-package actions: parse → expand `#` macros → collect signatures → run attached macros → type-check (dependencies via export data) → GIR → **write export data** → codegen. The macro steps follow P09 D9.3.
- The package's Clang worker answers its C/C++ header lookups (D6.4).
- Export data is written *before* codegen, so dependents start type-checking while the dependency is still in LLVM (like Rust's pipelined metadata). On deep graphs this is a large parallelism win.

**D6.2 — Export data (`.gopx`).**
- Versioned, binary and mmap-able, with an index so importers decode only the declarations they actually use (like Go's unified IR).
- GIR bodies use the encoding fixed in P04 D4.15 (LLVM bitstream container, same compiler version only, deterministic bytes).
- Three sections, each with its own hash:
  1. **Type API:** everything importers need to type-check (types, signatures, constants, ownership metadata, macro definitions, foreign-import references). No bodies, no positions. Its hash is the package's **API fingerprint**. It decides whether dependents are re-type-checked.
  2. **Bodies:** GIR for generic and inlinable functions. A dependent re-runs codegen only if a body it actually used changed. Each dependent records which bodies it used.
  3. **Positions:** stored relative to their declaration, and not hashed.
- Also stored:
  - attributes inferred from bodies and used by other packages (such as `nounwind`, and which parameters a function keeps, P01 D1.11); they are part of the API hash;
  - lowered C ABI signatures and symbol names, so plain C calls need no Clang in importers;
  - a header "recipe" for every foreign type mentioned (P07 D7.1).

**D6.3 — Content-addressed cache (`$GOPPCACHE`, default `~/.cache/gopp`).**
- Action ID = SHA-256 of:
  - the gopp version;
  - the package's **effective profile settings** (D6.12), not the profile's name: two profiles with the same settings share cache entries. The name is part of the key only when the package reads it (`#Profile()`, a `//gopp:build` profile term) and for cdeps (P11 D11.5);
  - the target and source hashes;
  - the dependencies' API fingerprints; for codegen, also the hashes of the dependency bodies that the package used (D6.2);
  - the hashes of the comptime GIR that the package's macro expansions executed (P09 D9.6), so editing a macro rebuilds the packages that expanded it;
  - Clang's list of header dependencies;
  - the relevant environment.
- Outputs are stored by content hash and trimmed LRU under a size cap.
- The same cache holds C/C++ artifacts (P07/P11) and macro expansions (P09).
- The cache is global and **not** part of `target/` (D6.12). All projects and worktrees on the machine share it.
- The module cache is a separate directory (P10 D10.5). Trimming and `gopp clean -cache` never touch it.

**D6.4 — In-process parallel scheduler.**
- Actions form a DAG, scheduled critical-path-first on a thread pool sized to the cores.
- Compile actions run in-process on threads, one `LLVMContext` each, so LLVM and Clang are never re-initialized per package.
- Only linking and CMake run as subprocesses.
- **Threading model:**
  - one Clang worker per package action (P07 D7.1), because Clang's Sema is not thread-safe;
  - one TargetMachine per thread;
  - no `cl::opt` changes after startup.

**D6.5 — Parallelism inside a package.**
- Function bodies are type-checked in parallel (P03 D3.4).
- At `opt 0` (and `opt 1`) the LLVM module is split into partitions along call-graph clusters, and the partitions are compiled in parallel.
  - The partition count depends on code size, not on `-p`, so the output is deterministic.
  - All Clang-generated code goes into one partition.
- Builds at `opt 2` and above keep one module per package to maximize inlining; ThinLTO (`lto thin`) handles cross-module inlining.

**D6.6 — Codegen pipelines are defined here; the `opt` setting picks one.** The old "build modes" (`debug`, `dev`, `release`) are replaced by build profiles (D6.12). A profile's `opt` value selects the pipeline:
- `opt 0` (default of the `debug` profile): O0 codegen plus a small curated IR pass set (SROA/mem2reg, instcombine, simplifycfg, always-inline). Targets (provisional): ≤ 1.5× the plain O0 + FastISel compile time, ≥ 3× its runtime speed. This is the old `dev` pipeline, which stays the default for `gopp build`.
- `opt 1`, `opt 2`, `opt 3`, `opt s`, `opt z`: the matching LLVM `-O1`, `-O2`, `-O3`, `-Os`, `-Oz` pipelines. `opt 3` is the default of the `release` profile.
- The plain **O0 + FastISel** pipeline (no GIR inlining, the old `debug` mode) is not a profile setting. It stays as the baseline for the D6.8 budgets and the D6.9 fallback, and is chosen with `-pipeline=fastisel` (a measuring and debugging flag).
- Everywhere else in the plan, "debug/dev build" means `opt 0` or `opt 1`, and "release build" means `opt 2` or higher.

**D6.7 — Measure, don't guess.**
- `gopp build -trace=out.json` writes a Chrome trace with per-action and per-LLVM-pass timings (LLVM `TimeProfiler`).
- `bench/compile` contains a generator that emits *equivalent Go and Go++ projects* (shared syntax subset; 10 k, 100 k and 1 M lines) for an apples-to-apples comparison with `go build`, plus a few real programs.

**D6.8 — Budgets** (provisional; P06.01 calibrates them against `go build` on the same machine).

| Scenario | Budget |
|---|---|
| Clean `debug` profile build, 100 k-line generated project | ≤ 2× `go build` |
| Clean `debug` build, generic-heavy generated project | set in P06.01 |
| Clean `debug` build, C++-heavy project (100 packages using `<vector>`, `<string>`, `<map>`) | set and checked once P08 lands (P08.13) |
| No-op rebuild | ≤ 100 ms |
| Edit one function body in a leaf package | ≤ 300 ms + link |
| Edit → running binary, including link | set in P06.01 |
| `gopp check` on 100 k lines | ≤ 1 s |

- **Conditions:** warm toolchain caches (the runtime, libc++, compiler-rt and musl come prebuilt in the release archive) and a warm module cache.
- **Leaf package:** a package that no other package in the project imports.
- Budgets are measured with 8 workers (`-p 8`), so they reflect a typical developer machine rather than the 192-core host.
- CI fails on a regression above 10%.

**D6.9 — Fallback, only if the debug budget is unreachable with LLVM `-O0`.**
- Evaluate a baseline backend for **debug builds only**, e.g. direct GIR → machine code for x86-64/arm64.
- Size: XL.
- It must emit exception tables (LSDA), unwind info (CFI) and DWARF, and match Clang's C ABI, so panics, C++ exceptions, debuggers and C calls keep working.
- LLVM remains the only backend for every profile and stays mandatory (R11).
- This work doesn't start unless P06 measurements demand it. The P04.15 spike did not (ADR-0028): EH costs FastISel 1.3 to 1.4 times the codegen time after the D4.6 lowering rules, about 0.37 s of CPU per 100k lines at `opt 0`.

**D6.10 — Verification model.** Task P06.12 builds it.
- Hashes are checked when a module or cdep tree is downloaded and extracted into the read-only cache.
- Each build only checks that gopp.sum has every entry and that the cached trees are marked verified.
- `gopp mod verify` re-hashes on demand.
- Tags are checked only by `gopp get` and `gopp mod tidy`.
- A file-hash cache keyed by (path, size, mtime, inode) speeds up hashing of local files.

*Why:* re-hashing every dependency on every build would break the no-op rebuild budget (D6.8).

**D6.11 — LSP support.** Task P06.13 builds it.
- **Overlays:** the driver can read unsaved editor buffers instead of the files on disk.
- **Check sessions:** a "check session" API that the LSP uses to check packages with the overlays applied (P14 D14.3).

**D6.12 — Build profiles.** (ADR-0007.) Tasks P06.14 to P06.16. The `profile` block grammar in gopp.mod is P10 D10.11; the CMake side is P11 D11.13; compile-time access is P09 D9.12.

A **profile** is a named set of nine build settings. Three are built in: `debug`, `release`, and `host`, which is used only for host builds of generator tools (P11 D11.12, ADR-0012). Their settings:

| Setting | Values | `debug` | `release` | `host` |
|---|---|---|---|---|
| `opt` | `0` `1` `2` `3` `s` `z` | `0` | `3` | `2` |
| `debuginfo` | `none` `line` `full` | `full` | `line` | `none` |
| `lto` | `off` `thin` `full` | `off` | `thin` | `off` |
| `panic` | `unwind` `abort` | `unwind` | `unwind` | `unwind` |
| `sanitize` | `none`, or a list of `address` `undefined` `thread` (`memory` also) | `none` | `none` | `none` |
| `strip` | `false` `true` (`true` = `symbols`: symbol table and debug info removed; finer value: `debuginfo`) | `false` | `true` | `false` |
| `cpu` | `baseline` `native` or a CPU name | `baseline` | `baseline` | `baseline` (of the host) |
| `pgo` | `off` `generate` or the path of a `.profdata` file | `off` | `off` | `off` |
| `link` | `static` `dynamic` | `dynamic` | `static` | `static` |

- `host` can be changed with a `profile host ( … )` block like the other two, but it can't be selected with `-profile=host` or used in `inherits`.

- `baseline` is the target triple's default CPU, so a binary runs on every CPU of that architecture. `native` is turned into the real CPU name before it is used, and that name (not the word `native`) goes into cache keys.
- `strip true` means `symbols`: it removes the symbol table and debug info from the binary, but does not remove the runtime line table that panic traces use (P05 D5.7).
- `link` is for cdeps (P11 D11.13): `debug` prefers shared libraries (faster relinks), `release` static ones. It is a preference only: a cdep that builds just the other kind gets that kind, with no error. An explicit `mode` in a cdep block is strict. Go++ code is always linked into one binary.
- A profile can set `sanitize` to a list. Combinations that the sanitizers do not support (for example `address` with `thread`) are an error (P05 D5.9).

**Custom profiles.** A profile with another name must say `inherits NAME` and may change any setting; the rest comes from the parent. Writing a `debug` or `release` block changes only the lines it writes. Example (P10 D10.11 has the grammar; the built-in `release` already has `opt 3` and `lto thin`):

```
profile fast (
    inherits release
    lto full
    cpu native
)
```

**Dependencies.** `deps ( … )` rows inside a profile give dependencies other settings than your code gets. For example, the `debug` profile can build all dependencies at `opt 2`. Only `opt` and `debuginfo` may be set per dependency; every other setting is whole-program (a mixed `panic`, `sanitize`, `lto` or `link` would not work). Details: P10 D10.11.

**Choosing a profile.**

| Command | Profile |
|---|---|
| `gopp build` (also `run`, `test`, `check`) | `debug` |
| `gopp build -release` | `release` (same as `-profile=release`) |
| `gopp build -profile=fast` | the profile named `fast` from the main gopp.mod |
| `gopp build -opt=2` | the chosen profile with `opt` changed for this command |

- **One-off overrides:** every setting has a flag with its own name: `-opt=`, `-debuginfo=`, `-lto=`, `-panic=`, `-sanitize=`, `-strip=`, `-cpu=`, `-pgo=`, `-link=`. Nothing is written to gopp.mod.
- **Existing flags:** `-panic=abort` is the `-panic=` override above. `-race`, `-asan`, `-ubsan` and `-msan` stay as shorthands that **add** `thread`, `address`, `undefined` or `memory` to `sanitize` (P05 D5.9). Their cache keys do not change shape: sanitizers are one of the profile settings in the key. `gopp test -race` and `-asan` work as before.
- `-release` together with a different `-profile=` is an error. An unknown profile name is an error that lists the known ones.
- **Order, weakest to strongest:** the built-in defaults, the `inherits` chain (parent first), the profile's own lines, its `deps` rows (for dependencies only), a cdep's own `profile ( … )` pin (P11 D11.13), then command-line overrides. A flag wins everywhere, also over `deps` rows and pins, so `-opt=0` really means `opt 0` for every package.
- **Only the main module's profiles count.** `profile` blocks in dependencies' gopp.mod files are ignored. Dependencies are built with the main module's profile.

**A profile never changes what a program means.** Language semantics are identical in every profile (README principle 4):
- Bounds, nil, division and shift checks, zero values, defined integer overflow and ownership rules are the same in every profile (P04 D4.8, P05 D5.8). Only `@NoBounds` turns a check off, per function.
- `opt`, `debuginfo`, `lto`, `strip`, `cpu`, `pgo` and `link` change speed, size and debuggability only.
- **`panic abort` is the one exception.** A panic prints its trace and ends the process (exit code 2) without unwinding, so `recover()` always returns nil (P04 D4.6). Code that depends on `recover` behaves differently, and it is the only setting that can do this.
- **`sanitize`** fits the principle because sanitizers only watch for undefined behavior (P01 D1.4 rule 8, spec chapter 16). A program without UB behaves the same, only slower and with more memory. A program with UB may now stop with a report (and the LSan leak check can change the exit code at exit): that is the point of the flag, and UB has no defined behavior to preserve.
- The program can still ask: `#Profile()`, `#Setting("name")`, `@If` (P09 D9.12) and `//gopp:build debug` (below) let the author write different code per profile. That is the author's choice, not the compiler's.

**Profile names in `//gopp:build`.** A term that equals a profile name is true when it is the main module's active profile: `//gopp:build debug` selects a file only in the `debug` profile, and `//gopp:build !debug` in all others. The expression syntax is Go's. A profile name that equals a GOOS, GOARCH or build-tag word (`linux`, `amd64`, `cgo`, …) is an error in gopp.mod. A custom profile `fast` makes `release` false, because only the exact active name matches. The loader (P03.01) evaluates the term, so the profile is known before any file is parsed, and the selected files are part of the package's source hash.

**Output layout: `target/`.** Like Cargo, each profile gets its own folder in the main module's root (next to gopp.mod):

```
myapp/
  gopp.mod
  main.gopp
  target/
    debug/
      myapp           the binary (and, for link dynamic, the shared libraries, P11 D11.4)
      tests/          test binaries from gopp test
      cdeps/          this profile's CMake build trees, one per cdep (P11 D11.13)
    release/
      …
```

- `-o` still names the binary yourself. `gopp run` runs the binary in `target/<profile>/`.
- `-target-dir DIR` or the environment variable `GOPPTARGETDIR` replaces `./target` (the flag wins). `gopp env` prints it.
- `gopp clean` deletes the target dir; `gopp clean -profile=NAME` only that profile's folder. `-cache` and `-modcache` are as before (D6.3, P10 D10.5).
- `gopp new` adds `target/` to the new project's `.gitignore` (P14 D14.1).
- **The cache stays global.** Compiled packages, export data, C/C++ artifacts and macro expansions live in `$GOPPCACHE` (`~/.cache/gopp`, D6.3), content-addressed and shared by all projects and all worktrees. `target/` only holds outputs for you (the linked binary, test binaries, CMake build trees). Nothing in `target/` is an input to a build, so deleting it is always safe, and a fresh worktree compiles nothing it has already seen: it only links.

## Deliverables

- `compiler/lib/Build`: graph, scheduler, cache, `.gopx` writer and reader.
- `gopp build/run/test/clean/env` with Go-compatible flags: `-o`, `-p`, `-v`, `-x`, `-tags`, `-trimpath`, and later the sanitizer flags `-race`, `-asan`, `-ubsan`, `-msan` (shorthands for `-sanitize`, D6.12).
- Build profiles (D6.12): the nine settings, `-release` / `-profile=` / one-off overrides, profile terms in `//gopp:build`, and the `target/<profile>/` layout.
- `-trace`.
- `bench/compile` with the generator, plus a CI budget job.

## Exit criteria

- M3: the multi-package samples build, and a no-op rebuild is all cache hits.
- Editing the body of a function that is neither generic nor exported for inlining doesn't rebuild dependents, unless a macro expansion in a dependent executed it.
- The D6.8 budgets are met (the C++-heavy one is checked in P08.13), or an ADR documents each gap with a plan.
- Profiles: `debug` and `release` builds of one project sit side by side in `target/`; changing one setting rebuilds only what depends on it; a second worktree of the same project builds from cache hits.

## Risks

- **LLVM `-O0` still too slow** → D6.5 partitioning, leaner GIR, the D6.9 fallback.
- **Export-data format churn** → a version field; a mismatch triggers a full rebuild.
- **Cache staleness or poisoning** →
  - action IDs include everything that can affect the output, including the effective profile settings;
  - `gopp build -a` forces a rebuild;
  - `GOPPCACHEVERIFY=1` re-runs actions and compares their outputs.
- **Clang threading** (Clang's Sema is not thread-safe) → one Clang worker per package (D6.4, P07 D7.1).

## Open questions

- **Q1** (decided 2026-10-27) Per-function incremental compilation is not in v1; research after v1.
- **Q2** (decided 2026-10-27) A distributed or remote cache is not in v1; it may come after v1.
- **Q3** (decided 2026-10-27) In-process LLD vs a subprocess: a subprocess. `gopp` runs its own bundled LLD as a child process (`gopp lld …`), so there is still one file to ship. Each link is a fresh process: an LLD crash or fatal error cannot take `gopp` down, long-running `gopp` processes (LSP, watch, the test runner) keep no LLD state, and many links run in parallel (`gopp test`). The ~1–3 ms process start per link is accepted. Library-mode LLD is reconsidered only if the P06 measurements show the start cost matters (P04 D4.9).
