# STATE: Go++ language plan

Legend: `[ ] not_started` · `[~] in_progress` · `[x] completed`
Last updated: 2026-10-27

## Plan

- [x] Plan drafted: README, 17 phases (README + STATE each), TODO
- [x] Plan reviewed and accepted by the owner

## Phases

- [ ] P00 Foundations ([state](00-foundations/STATE.md))
- [~] P01 Language specification v0 ([state](01-language-spec/STATE.md))
- [ ] P02 Front-end: lexer, parser, AST, `gopp fmt` ([state](02-frontend/STATE.md))
- [ ] P03 Semantic analysis ([state](03-semantic-analysis/STATE.md))
- [ ] P04 GIR and LLVM codegen ([state](04-gir-and-llvm-codegen/STATE.md))
- [ ] P05 Runtime and memory ([state](05-runtime-and-memory/STATE.md))
- [ ] P06 Build driver and compile speed ([state](06-build-driver-and-compile-speed/STATE.md))
- [ ] P07 C interop ([state](07-c-interop/STATE.md))
- [ ] P08 C++ interop ([state](08-cpp-interop/STATE.md))
- [ ] P09 Macros ([state](09-macros/STATE.md))
- [ ] P10 Modules: gopp.mod / gopp.sum ([state](10-modules-gopp-mod-sum/STATE.md))
- [ ] P11 C/C++ deps from the internet + CMake ([state](11-c-cpp-deps-and-cmake/STATE.md))
- [ ] P12 Concurrency ([state](12-concurrency/STATE.md))
- [ ] P13 Standard library v1 ([state](13-standard-library/STATE.md))
- [ ] P14 Tooling ([state](14-tooling/STATE.md))
- [ ] P15 Performance ([state](15-performance/STATE.md))
- [ ] P16 Release ([state](16-release/STATE.md))

## Milestones

- [ ] M0 Hello, LLVM: one-file `package main` → native binary via LLVM + LLD (P00, P02–P04 slice)
- [ ] M1 Go core: ≥ 100 differential tests vs `go run` pass (P02–P04)
- [ ] M2 No-GC core: ownership, destructors, `!`, panic/defer/recover; ASan/LSan clean (P05)
- [ ] M3 Fast builds: export data + cache; compile budgets enforced in CI (P06)
- [ ] M4 C bridge: C header import, package-local C, `gopp cc` (P07)
- [ ] M5 C++ bridge: std containers, templates, RAII, exceptions ↔ panics (P08)
- [ ] M6 Macros: declarative + procedural + `@Derive` (P09)
- [ ] M7 Packages: Go++ modules + internet C/C++/CMake deps locked in mod + sum (P10, P11)
- [ ] M8 Concurrency: goroutines, channels, select; TSan clean (P12)
- [ ] M9 v0.1 alpha: std v1, tooling, perf targets, release (P13–P16)

## Current focus

- Next: **P00.01 Initialize repository**. The owner is asked before the first commit.
- Parallel tracks once P00 is done: P01 (spec) is the critical path. P00 CI/test-harness work can proceed alongside it.

## Log

- 2026-10-27: Plan created. Environment observed: NixOS 26.05, 192 cores, clang 21.1.8, CMake 4.1.2, Ninja 1.13.2, Go 1.25.7, mold 2.40.4. LLVM dev libraries and LLD are not installed (handled in P00.02/P00.03).
- 2026-10-27: Pre-review fix (owner request): attached macro renamed `@derive` → `@Derive` across the plan.
- 2026-10-27: Pre-review fixes (owner decisions): every macro and attribute name is capitalized (`#Assert`, `@Extern`, `@Derive`, …); `fmt` and `encoding/json` work on any type via compiler-emitted field tables (Go-like), with `@Derive(JSON)` as an optional fast path; std derive set reduced to `JSON` and `Equal` (`Clone` removed; `clone()` stays automatic per D1.4).
- 2026-10-27: Pre-review fix (owner decision): Must moved from postfix `call()!` to `call!()`. Go's `val, err := call()` stays as-is; `!` applies to calls only (map/receive `!` forms dropped; use comma-ok).
- 2026-10-27: Pre-review fixes (owner decisions): C/C++ package headers are imported Go-style (`import "github.com/madler/zlib/zlib.h"`; system headers keep `c:`/`c++:`); cdep `target` replaced by `mod static|dynamic`; CMake only (`build` key and `sources`/`headers` recipes removed); packages without CMake use a local CMake file (`cmake ./cmake/X.cmake`); archive packages get a Go-style path plus `url`.
- 2026-10-27: Pre-review fixes (owner decisions): cdep key renamed `mod` → `mode`; system headers imported as `import "<vector>"` / `import "<stdio.h>"` (`c:`/`c++:` removed); no `C.`/`cpp.` pseudo-packages: each imported header is a package named after the file (`zlib.crc32`, `vector.vector[int32]`, `algorithm.sort`), C++ namespaces flattened; C types map to plain Go++ types; strings and slices pass to C without conversions; ABI-only helpers in std package `ffi`.
- 2026-10-27: Pre-review fix (owner decision): Go's `new` removed; allocation is explicit with `new_unique(v)` → `Unique[T]` and `new_shared(v)` → `Shared[T]`, and `new_weak(s)` makes a `Weak[T]` from a `Shared[T]`; `unique *T`/`shared *T`/`weak *T` replaced by the predeclared generic types `Unique[T]`/`Shared[T]`/`Weak[T]` (keywords `unique`/`shared`/`weak` removed); `&T{…}` is now a compile error with a fix-it.
- 2026-10-27: Pre-review fix (owner decision): `func f!(…) T` declares a function that can fail (callers see `(T, error)`); `call!()` returns the error inside functions that can fail and panics elsewhere (e.g. `main`); Go-style `val, err := call()` unchanged; errors returned through `!` carry a trace (P05.14); `#Try` macro dropped.
- 2026-10-27: Deep review started: four independent read-only reviewers (language & memory model; C/C++ interop & dependencies; compiler, build speed & phase plan; document consistency).
- 2026-10-27: Deep review finished: 89 findings from 4 reviewers, merged into 57 items (blockers: owned reads from fields, interfaces consuming values, error idioms, bootstrap order, CMake provider, transitive cdeps, CMake File API). Waiting for owner decisions before applying fixes.
- 2026-10-27: Review fixes applied to every phase.
  - Owner decisions: no macOS in v0.1 (Linux amd64/arm64 only); `s = s[i:j]` shrinks an owned slice in place, slices stay 3 words, std gets `container/deque`; errors work like Go (shared values, `err == io.EOF`); no extra memory-safety layer in v1 (known dangling cases listed as UB).
  - Coordinator calls in the owner's "like Go" direction, waiting for the owner's OK:
    1. interface values (including `error`) and func values are shared, reference-counted handles;
    2. escaping closures keep captured variables in shared cells, chosen by Go-style escape analysis (also across calls, e.g. `http.HandleFunc`);
    3. taking ownership of a value read from a field, element, map entry or other non-local place needs `clone(x)` or `move(x)`; just reading it borrows.
  - 15 new decisions: D0.12, D1.19–D1.21, D4.11–D4.13, D5.12, D6.10–D6.11, D9.11, D10.10, D11.10–D11.12. 24 new tasks (228 in total). Every phase has numbered open questions, mirrored in its STATE.md.
  - Follow-ups from the writers' reports: `gpp fmt` sorts only import groups without header imports (D2.8); seccomp blocks the network in the cdep sandbox when namespaces are unavailable (D11.3); `tidy` refreshes `// indirect` cdep blocks (D10.3); Nix workflow (`flake.lock`, `.envrc`, `nix develop -c`, D0.4) and a flake package for NixOS (D16.4).
  - Checks pass: TODO.md and every STATE.md list the same task IDs; every decision and open-question reference resolves.
- 2026-10-27: Owner approved the three coordinator calls: shared interface, `error` and func values; Go-style boxes for closures that outlive their function; `clone(x)`/`move(x)` to take ownership of a value read from a field, element or map entry.
- 2026-10-27: Consistency review of the applied fixes found 13 must-fix items, all fixed:
  - interface parameters are shared handles (`return err` works as in Go);
  - substrings and borrowed `string` parameters pass to C as temporary copies;
  - every C++ move runs the move constructor; only containers relocate;
  - goroutine switches save the C++ exception state;
  - macro bodies are part of dependents' cache keys;
  - one header-extension list; header paths are source-tree paths;
  - cdep target choice filters by `mode` first;
  - new P12 D12.12 and task P12.12: stopping goroutines at exit (229 tasks);
  - tasks moved to the phase where their dependencies exist (P04.03, P04.08, P05.12, P06.12, P07.08);
  - `/notfound` gpp.sum lines; `readonly` never on `[]T` data.
- 2026-10-27: Re-check returned CLEAN. Its 4 minor follow-ups are fixed (phase dependency links, the C++-heavy budget, non-clonable interface arguments, goroutines stopped before `main`'s locals on a panic). Plan accepted.
- 2026-10-27: Owner decisions on the P00 and P01 open questions:
  - P00 Q1: GitHub + GitHub Actions. Q2: Apache-2.0 WITH LLVM-exception. Q3: the language stays Go++, and everything else is renamed to `gopp` (CLI `gopp`, `.gopp` files, `gopp.mod`/`gopp.sum`, `.gopx`, `//gopp:` directives, `GOPP*` variables). Log entries above keep the old names.
  - P01 Q4: every owned slice and map stores its allocator, so owned slices are 4 words (new D1.22; replaces "slices stay 3 words"). Q5: generic methods allowed (D1.10). Q6: one-word interface inline storage (D1.12).
  - P01.12 is done, so P01 is in progress.
- 2026-10-27: Fast dev loop for `gopp` itself (owner request): README principle 7; P00 D0.13 with budgets (edit → rebuilt ≤ 10 s, stage tests ≤ 10 s, full check ≤ 5 min, one test ≤ 2 s, clean build ≤ 2 min) and tasks P00.13–P00.14; P04 D4.14 fast test runner (batch compiles, ORC JIT in executor processes, Go outputs saved as golden files) and task P04.19; agent rules added to the repo's `CLAUDE.md`. 232 tasks.
