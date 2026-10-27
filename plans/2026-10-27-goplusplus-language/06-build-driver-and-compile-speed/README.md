# P06 — Build driver and compile speed

**Goal:** `gopp build/run/test` for multi-package programs at Go-like speed: export data instead of headers, a parallel and pipelined package graph, content-addressed caching, and a cheap debug pipeline. Compile-speed budgets are measured and enforced in CI.
**Depends on:** P04, P05 · **Unblocks:** P07, P09, P10, P11 · **Size:** L (budgets stay enforced until v1) · **Milestone:** M3

## Where compile time goes, and the answer

| Cost | Go's answer | Go++'s answer |
|---|---|---|
| Re-reading dependencies | export data | export data (`.gopx`), lazily decoded |
| C/C++ headers | — (cgo is slow) | import units cached as PCH + binding export data (P07) |
| Generic code | GC-shape stenciling | monomorphization + instance cache + COMDAT folding |
| Backend | its own fast backend | LLVM: O0 + FastISel (debug), a tuned small pipeline (dev), parallel codegen |
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
  - the gopp version and build configuration;
  - the target and source hashes;
  - the dependencies' API fingerprints; for codegen, also the hashes of the dependency bodies that the package used (D6.2);
  - the hashes of the comptime GIR that the package's macro expansions executed (P09 D9.6), so editing a macro rebuilds the packages that expanded it;
  - Clang's list of header dependencies;
  - the relevant environment.
- Outputs are stored by content hash and trimmed LRU under a size cap.
- The same cache holds C/C++ artifacts (P07/P11) and macro expansions (P09).
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
- In debug/dev modes the LLVM module is split into partitions along call-graph clusters, and the partitions are compiled in parallel.
  - The partition count depends on code size, not on `-p`, so the output is deterministic.
  - All Clang-generated code goes into one partition.
- Release builds keep one module per package to maximize inlining; ThinLTO handles cross-module inlining.

**D6.6 — Build modes are defined here.**
- `debug`: O0 + FastISel.
- `dev` (default): O0 codegen plus a small curated IR pass set (SROA/mem2reg, instcombine, simplifycfg, always-inline). Targets (provisional): ≤ 1.5× debug compile time, ≥ 3× debug runtime speed.
- `release`: O2 (O3 opt-in), optional ThinLTO.

**D6.7 — Measure, don't guess.**
- `gopp build -trace=out.json` writes a Chrome trace with per-action and per-LLVM-pass timings (LLVM `TimeProfiler`).
- `bench/compile` contains a generator that emits *equivalent Go and Go++ projects* (shared syntax subset; 10 k, 100 k and 1 M lines) for an apples-to-apples comparison with `go build`, plus a few real programs.

**D6.8 — Budgets** (provisional; P06.01 calibrates them against `go build` on the same machine).

| Scenario | Budget |
|---|---|
| Clean `debug` build, 100 k-line generated project | ≤ 2× `go build` |
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
- LLVM remains the only backend for dev and release builds and stays mandatory (R11).
- This work doesn't start unless P06 measurements (or the P04.15 spike) demand it.

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

## Deliverables

- `compiler/lib/Build`: graph, scheduler, cache, `.gopx` writer and reader.
- `gopp build/run/test/clean/env` with Go-compatible flags: `-o`, `-p`, `-v`, `-x`, `-tags`, `-trimpath`, and later `-race`.
- `-trace`.
- `bench/compile` with the generator, plus a CI budget job.

## Exit criteria

- M3: the multi-package samples build, and a no-op rebuild is all cache hits.
- Editing the body of a function that is neither generic nor exported for inlining doesn't rebuild dependents, unless a macro expansion in a dependent executed it.
- The D6.8 budgets are met (the C++-heavy one is checked in P08.13), or an ADR documents each gap with a plan.

## Risks

- **LLVM `-O0` still too slow** → D6.5 partitioning, leaner GIR, the D6.9 fallback.
- **Export-data format churn** → a version field; a mismatch triggers a full rebuild.
- **Cache staleness or poisoning** →
  - action IDs include everything that can affect the output;
  - `gopp build -a` forces a rebuild;
  - `GOPPCACHEVERIFY=1` re-runs actions and compares their outputs.
- **Clang threading** (Clang's Sema is not thread-safe) → one Clang worker per package (D6.4, P07 D7.1).

## Open questions

- **Q1** (decided 2026-10-27) Per-function incremental compilation is not in v1; research after v1.
- **Q2** (decided 2026-10-27) A distributed or remote cache is not in v1; it may come after v1.
- **Q3** In-process LLD vs a subprocess (measure; see P04 D4.9).
