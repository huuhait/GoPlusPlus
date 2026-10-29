# P15 — Performance

**Goal:** prove "fast like C++, or almost" with a benchmark suite against C++, Go and Rust. Do the optimization work in GIR and the LLVM pipeline, add LTO and PGO, and enforce performance budgets in CI.
**Depends on:** P04–P08, P11, P13 (P12 for concurrent benchmarks) · **Size:** M, then ongoing · **Milestone:** M9

## Decisions

**D15.1 — Benchmark suite (`bench/runtime`).**
- **Programs:**
  - compute-bound: nbody, spectral-norm, fannkuch-redux, mandelbrot, fasta;
  - allocation-heavy: binary-trees;
  - map-heavy: k-nucleotide;
  - I/O: reverse-complement;
  - JSON parse and serialize;
  - regex-redux through a C PCRE2 import (doubles as an interop benchmark);
  - a `std::vector`-heavy kernel through C++ interop;
  - a goroutine/channel pipeline.
- **Implementations:** each program is written idiomatically in Go++, C++ (clang++ `-O2` with the same LLVM), Go and Rust. Outputs are cross-verified.

**D15.2 — Targets (provisional).**
- Geomean runtime ≤ 1.10× C++.
- No single benchmark slower than 1.5× C++.
- At least as fast as Go on every allocation-heavy benchmark.
- Static binary size within 1.5× of the equivalent C++. The size counts the runtime line table (P05 D5.7).

**D15.3 — Planned optimizations, roughly by expected payoff.**
1. **Allocation elision.** Mark allocator calls with LLVM `allocsize`/`allockind`/`alloc-family` attributes so LLVM can remove or stack-promote matching alloc/free pairs.
2. **Ownership-derived attributes.** Each attribute is emitted only where a language rule guarantees it, and each one gets a miscompile test.

   | Attribute | Where | Why |
   |---|---|---|
   | `nonnull`, `dereferenceable`, `noundef` | hidden pointers | the compiler creates them, and they always point at valid memory |
   | `readonly` | hidden pointers of borrowed owning parameters, and `string` data | P01 D1.5 forbids top-level writes and `&param`, and string bytes never change. Never on `[]T` data: its elements stay mutable |
   | `noalias` | **never** on `Unique[*T]` | raw aliases are legal |
   | `noalias` | `move` parameters, only when the caller passes a fresh value | otherwise other pointers to the value can exist |
   | `captures(none)` (formerly `nocapture`) | only where no view to the parameter can escape (not for functions that return views) | a returned view lets the parameter escape |

3. **Check elimination.** GIR range facts plus LLVM IndVarSimplify/IRCE remove bounds and nil checks.
4. **Devirtualization.**
   - At the GIR level when the dynamic type is known.
   - LLVM whole-program devirtualization with type metadata under LTO.
5. **Inlining.**
   - Small functions inline across packages via GIR bodies in export data (release builds).
   - ThinLTO across the whole program, including C/C++ cdeps built by `gopp cc` (cross-language LTO).
6. **PGO.**
   - `gopp build -pgo=<profile>` accepts instrumented or sampled (perf) profiles.
   - A `default.pgo` in the main package is used automatically, as in Go.
7. **Allocator.** Evaluate bundling mimalloc as the default (P15.10, Q2).
8. **Struct layout.** C compatibility rules out automatic field reordering. `gopp vet` suggests orderings that save padding (task P15.13).

**D15.4 — Perf CI.**
- Runs on a dedicated machine: fixed CPU frequency, isolated cores, several runs with the median reported.
- Results go to a dashboard.
- A regression above 3% on any benchmark raises an alert.

## Deliverables

- `bench/runtime`: four implementations per program, plus a harness.
- Perf CI and the dashboard.
- Each D15.3 optimization, with a FileCheck test and a recorded benchmark delta.
- ADRs for the allocator choice and for any missed target.

## Exit criteria

- D15.2 targets are met, or each gap is documented with a root cause and an ADR.
- The perf dashboard is live and regression alerts work.

## Risks

- **Go semantics cost performance relative to C++.** Sources: bounds checks, wrapping overflow (no `nsw`), zero initialization, interface boxing, unwinding tables. Mitigations:
  - measure each cost separately;
  - offer explicit opt-outs (`@NoBounds`, `unsafe`, `-panic=abort`);
  - document what remains.
- **Benchmark gaming** → idiomatic implementations only, reviewed by people fluent in each language.

## Open questions

- **Q1** Small-string optimization for `string` (from P01 Q8)? The layout is ABI, so decide before v1.0 (task P15.14).
- **Q3** SIMD: **deferred by owner 2026-10-28.** Not designed yet; recorded so it is not forgotten. No task, no spec rule. Decide before v1.0 only if the owner wants it.
  - *Today:* LLVM auto-vectorization in release builds (the D15.3 pipeline), plus C/C++ intrinsics and libraries through header imports (P07, P08).
  - *Option A:* nothing new.
  - *Option B:* a portable `simd` package with `simd.Vec[T, N]` and the normal operators (possible now through operator overloading, ADR-0024); bounds-checked `Load`, `Store` and `LoadPartial`; `Min`, `Max`, `Abs`, `Sqrt`, `FMA`, `SatAdd`; `Sum` and `ReduceMax`; `Select` with `simd.Mask`; `Shuffle`, `Convert` and `Gather`; `simd.Native[T]` (the best width for the target) and `simd.Has(...)` (feature test); and `@Target("avx2", "avx512", "neon")` multiversioning with a run-time pick.
  - *Option C:* B plus raw `simd/x86` and `simd/arm` intrinsics packages.
  - *Comparison:* C++ has intrinsics, Highway and `std::simd`; Rust has `std::arch` and a nightly `std::simd`; Zig has `@Vector`; Go has none.
  - *Owner's recommendation note:* B now, with C later.
- **Q2** Should mimalloc be the default allocator (from P05 Q2)? Task P15.10 evaluates it (D15.3 item 7).
