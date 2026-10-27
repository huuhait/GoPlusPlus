# TODO: Go++ tasks by phase

Each task has a stable ID (`P03.07`). Its status lives in that phase's `STATE.md` (`[ ] not_started` · `[~] in_progress` · `[x] completed`). This file defines the work and says when each task counts as done. New tasks take the next free ID in their phase and must also be added to that phase's STATE.md.

---

## P00 — Foundations ([README](00-foundations/README.md))

- **P00.01 Initialize repository**: `git init`, the directory layout from D0.5 (`.gitkeep` in empty dirs), `.gitignore`, a top-level `README.md` pointing to this plan, and a CONTRIBUTING stub. *Done when:* the first commit exists.
- **P00.02 Nix dev shell**: a `flake.nix` providing:
  - the pinned LLVM/Clang/LLD development packages;
  - CMake, Ninja, ccache, mold;
  - Python 3 + lit;
  - Go 1.25 (for differential tests);
  - GoogleTest;
  - an optional shell with an assertions-enabled LLVM (D0.4);
  - a committed `flake.lock`, and an optional `.envrc` for direnv.

  *Done when:* `nix develop -c llvm-config --version` and `FileCheck --version` both work, in both shells.
- **P00.03 Pin the LLVM major (ADR-0002)**: pick the newest stable LLVM major in the pinned nixpkgs (≥ 21). Today clang 21.1.8 is installed, but the LLVM dev libraries and LLD are not. *Done when:* the ADR is merged and the flake is pinned.
- **P00.04 CMake skeleton**:
  - `find_package(LLVM)`/`find_package(Clang)` + LLD libraries;
  - one target per `compiler/lib/*`;
  - the RTTI flag taken from `llvm-config --has-rtti`, no exceptions (D0.3);
  - ccache, and mold or lld as linker.

  *Done when:* `ninja gopp` builds from a clean tree.
- **P00.05 `gopp version`**: prints the Go++ version, LLVM version and default target triple. *Done when:* a lit test checks the output.
- **P00.06 Test harness**: `tests/lit.cfg.py` (substitutions `%gopp`, `FileCheck`), GoogleTest wiring, a `ninja check` target. *Done when:* one lit test and one unit test pass.
- **P00.07 Code style**: `.clang-format` (LLVM style), `.clang-tidy`, a `ninja format-check` target. *Done when:* the check passes in CI.
- **P00.08 CI**: GitHub Actions on Linux x86-64 and arm64 runners only, with these jobs running `ninja check`:
  - Release;
  - Debug+ASan/UBSan;
  - a build against the assertions-enabled LLVM.

  *Done when:* all jobs are green on main.
- **P00.09 ADR process**: `docs/adr/0000-template.md`, ADR-0001 (implementation language), ADR-0003 (memory model direction), ADR-0004 (`!` semantics). *Done when:* the ADRs are merged.
- **P00.10 License**: Apache-2.0 WITH LLVM-exception (D0.10, confirmed by the owner); add LICENSE and a file-header policy. *Done when:* LICENSE is committed.
- **P00.11 Naming record**: ADR-0005 records D0.8 (Go++, `gopp`), why `gpp` was dropped (an unrelated Linux tool), and the "Go" trademark risk. *Done when:* the ADR is merged.
- **P00.12 Host sysroot discovery** (D0.12):
  - ask the system C compiler (`cc -print-file-name=crt1.o`, `cc -E -v`) or read the Nix variables;
  - record the include dirs, crt objects and dynamic linker;
  - fall back to bundled static musl.

  *Done when:* the same probe finds glibc headers and `crt1.o` on this NixOS machine and in a Debian container.
- **P00.13 Fast compiler build** (D0.13): shared LLVM/Clang libraries in dev builds (static in release); precompiled headers for the codegen and interop libraries; dev build type `-O1 -g` + assertions; a CI check that front-end libraries include no LLVM IR or Clang headers. *Done when:* on the dev machine, editing one `.cpp` rebuilds `gopp` in ≤ 10 s, a clean build takes ≤ 2 min, and the header check runs in CI.
- **P00.14 Fast test loop** (D0.13): per-stage `check-*` targets, lit on every core, `check-long` for opt-in slow tests, a slowest-20 report, and a CI gate on suite time. *Done when:* the targets exist, every run prints its slowest tests, and the gate fails on a seeded 10% slowdown.

## P01 — Language specification v0 ([README](01-language-spec/README.md))

- **P01.01 Spec skeleton**: chapters in `docs/spec/`: overview, lexical, types, ownership, parameters, expressions, statements, errors-and-panics, generics, concurrency, macros-surface, foreign-surface, program-init, undefined-behavior. The base is the Go 1.25 spec (D1.1). *Done when:* the chapters exist with cross-references to the Go 1.25 spec.
- **P01.02 Lexical deltas**: keywords, `#`, `@`, `!` in `func f!()` and `call!()`, predeclared names (D1.2), EBNF. *Done when:* the chapter is complete with examples.
- **P01.03 Ownership chapter**:
  - type categories, including shared handles (D1.3);
  - owners, drop order, moves and `move`;
  - reading from non-local places (rule 3a), zero values, `Clone` (`x.Clone()`, short form `clone(x)`);
  - C++ class types (rule 9) and pinned values (rule 10);
  - heap allocation (`new_unique`, `new_shared`, `new_weak`; `&T{}` removed) and pointer kinds;
  - refs and the `append` rule.

  *Done when:* every rule is numbered and has an example and a rationale.
- **P01.04 Parameter passing**: owning parameters borrow (read-only at top level); trivial parameters are copies; `move` (before the name; owning variadics `move ...T`); `*T`; receivers; `&param`; ABI notes; generic examples with an owning `T`. *Done when:* the chapter is complete.
- **P01.05 `!` and errors chapter**:
  - `func f!()` declarations, literals and function types;
  - `call!()`: returns the error inside functions that can fail, panics elsewhere;
  - the exact failure semantics, and returns in `!` functions;
  - deferred `f!()`;
  - errors as shared values, and traces stored outside the error (D1.19);
  - the panic value `errors.MustError`;
  - `defer`/`go`, and the vet rule for `(n, err)` calls.

  *Done when:* the chapter is complete.
- **P01.06 panic/defer/recover**:
  - unwinding with destructors;
  - Go-exact nested panics, and panics in `drop` (abort);
  - `move` arguments on panic;
  - function-scoped defer and the defer-argument rule;
  - exit order;
  - forced unwinding and `longjmp`;
  - `-panic=abort`.

  *Done when:* the chapter is complete, including the loop-defer example (legal, as in Go, D1.9).
- **P01.07 Generics deltas**: `Copy` and `Clone` constraints (written like `comparable`), owning type arguments, `Clone` in generic returns, generic methods (D1.10). *Done when:* the chapter is complete.
- **P01.08 Concurrency semantics**: `go` argument rules, closures used with `go`, channel and `select` moves, program exit with running goroutines (D1.18), Go's memory model, races as UB. *Done when:* the chapter is complete.
- **P01.09 Closures chapter**: D1.11 — shared cells for escaping closures, method values, recursive closures, defer arguments. *Done when:* the rule has ≥ 6 examples, each compared with Go's behavior.
- **P01.10 Macro & attribute surface**: syntax, the built-in attribute list, and naming across packages (`#pkg.Name`, predeclared std macros, how `@Derive(X)` finds X). *Done when:* the chapter is complete and points to P09 for semantics.
- **P01.11 Foreign import surface**:
  - header imports (`"github.com/madler/zlib/zlib.h"`, `"<vector>"`) and how they are told apart from packages;
  - headers inside your own module;
  - header packages (`zlib.crc32`) and namespace flattening;
  - import order and `import _`;
  - `//gopp:` directives.

  *Done when:* the chapter is complete.
- **P01.12 Close open questions Q4, Q5, Q6**: decide each, or defer it to a named phase. *Done when:* every P01 open question is `[x]` in STATE.md.
- **P01.13 Ten sample programs**: `docs/spec/examples/*.gopp` per D1.16, each with its expected output and a Go counterpart for comparison. *Done when:* all ten are reviewed.
- **P01.14 UB & implementation-defined list**:
  - the Go-semantics cases from D1.17;
  - the dangling cases v1 does not catch (D1.4 rule 8);
  - data races;
  - `longjmp`.

  *Done when:* the chapter is complete.
- **P01.15 Spec review**: owner review of the 42 notes (decisions D1–D25 are recorded in `01-language-spec/STATE.md`); apply them to the spec, the plan and the ADRs; update ADR-0003/0004 and add ADR-0006. *Done when:* all review comments are resolved.
- **P01.16 Slices and range chapter**: re-slicing owned slices in place (D1.20), allocators in slices and maps (D1.22), `container/deque` for queues, `move dst []T` for append-style functions, range loops including the `&d` form (D1.21), and temporaries in statement headers (D1.6). *Done when:* the chapter is complete with examples.

## P02 — Front-end ([README](02-frontend/README.md))

- **P02.01 Source manager**: mmap'd files, 32-bit global offsets, lazy line tables, and a separate offset space per LSP snapshot. *Done when:* unit tests pass.
- **P02.02 Diagnostics engine**: severities, ranges, notes, fix-its, text and JSON output, error limit. *Done when:* lit golden tests pass.
- **P02.03 Lexer**: Go's tokens and literals plus the new tokens and keywords; Go's semicolon insertion. *Done when:* ported Go scanner tests pass.
- **P02.04 AST & arenas**: node set, arena allocation, identifier interning, dump printer. *Done when:* `--dump-ast` output is stable.
- **P02.05 Parser core**:
  - declarations;
  - types (`ref`, `Unique[T]`/`Shared[T]`/`Weak[T]`), the `move` parameter mark, and the `&` mark in range clauses;
  - statements and expressions;
  - `!` functions, literals, function types and calls (`func f!(…)`, `func!(int) T`, `f!(…)`, `parse[int]!(s)`);
  - generics.

  *Done when:* grammar-coverage tests pass.
- **P02.06 Parser: macros & attributes**: `#Name(...)`, `#pkg.Name(...)` and `@Name(...)` as token trees; `macro` definitions; attributes after a struct field's type (before the Go tag). *Done when:* tests pass.
- **P02.07 Error recovery**: sync points; tailored messages for the 20 most common mistakes, Go habits included. *Done when:* golden tests pass.
- **P02.08 `gopp parse`**: `--dump-ast`, `--dump-tokens`. *Done when:* the lit suite's AST golden files are produced with `gopp parse --dump-ast`.
- **P02.09 `gopp fmt`**: comment map, gofmt rules extended to the new syntax, `-l -w -d`. Header imports are never reordered. *Done when:* the round-trip invariant holds on the corpus, and a test shows header import order is kept.
- **P02.10 Parser fuzzing**: libFuzzer target, corpus, crash triage. *Done when:* 24 CPU-hours run without crashes.
- **P02.11 Parse throughput**: measure against the D2.9 budget. *Done when:* the budget is met or an ADR documents the gap.

## P03 — Semantic analysis ([README](03-semantic-analysis/README.md))

- **P03.01 Package loader**: directories, `//gopp:build` (including profile names, P06 D6.12), `_os_arch` file suffixes, `_test.gopp`, `internal/`, import cycles. *Done when:* tests pass.
- **P03.02 Constant arithmetic**: APInt plus a big-float spike (libbf vs alternatives). *Done when:* Go's constant test cases pass.
- **P03.03 Scopes & resolution**: universe, package, file and block scopes; the export rule and its foreign exemption; unused variables and imports. *Done when:* tests pass.
- **P03.04 Types core**: identity, assignability, conversions, method sets, embedding/promotion, interface satisfaction. *Done when:* ported types2 cases pass.
- **P03.05 Statements & expressions**: including range-over-int/func, switch/type switch, select, labels. *Done when:* tests pass.
- **P03.06 Generics**: type parameters, type sets, inference, instantiation checks, generic methods (P01 D1.10). *Done when:* ≥ 90% of the ported types2 generics cases pass.
- **P03.07 Type-level ownership**:
  - type categories and shared handles;
  - `move` mode (and `move ...T` for owning variadics), read-only owning parameters, trivial parameters as copies;
  - reading from non-local places (rule 3a: borrow, or `x.Clone()`/`move(x)` fix-its);
  - range variables: read-only borrows, the `&d` alias form, and the loop-variable redeclaration error;
  - ref conversions and borrow-of-temporary errors;
  - `move`/`clone` typing, `x.Clone()` and `IsNil()` method lookup, and the `append` rule;
  - `Copy`/`Clone` constraints;
  - returning a `ref` into a local;
  - `@Pinned`.

  *Done when:* rule-table tests pass.
- **P03.08 `!` typing**: signatures, calls, literals and function types, return forms. *Done when:* tests pass, diagnostics included.
- **P03.09 Foreign scope interface**: `ForeignScope` plus a mock. *Done when:* tests with fake C/C++ declarations pass.
- **P03.10 Parallel body checking**. *Done when:* diagnostics are deterministic, and the speedup is measured and recorded.
- **P03.11 Type serialization interface**: for export data (the format itself is P06). *Done when:* round-trip tests pass.
- **P03.12 `gopp check` & `gopp vet` v0**: `check` runs the type-level checks (P04.09 adds the GIR checks). First lints: ref-typed fields, drop-on-zero heuristic, a `!` that would panic outside `main`. *Done when:* CLI tests pass.
- **P03.13 Check throughput**: measure against the D3.10 budget. *Done when:* the budget is met or an ADR documents the gap.

## P04 — GIR and LLVM codegen ([README](04-gir-and-llvm-codegen/README.md))

- **P04.01 GIR design**: `docs/design/gir.md`; data structures, builder, verifier, printer. *Done when:* the verifier runs in all tests.
- **P04.02 M0 hello**: `println` via `@Extern("C") write`; LLVM IR → object → LLD, using host sysroot discovery (P00.12). *Done when:* hello runs from a lit test on NixOS and on a Debian container.
- **P04.03 Targets & linking**: triples, data layouts, TargetMachine, crt objects, LLD driver; linux/amd64 and linux/arm64, each built natively on its own CI runner (cross builds come with musl, P07.14). *Done when:* hello runs on both runners.
- **P04.04 Lower core**: scalars, control flow, functions, structs, arrays, raw pointers, and the Go-semantics lowering table (D4.12). *Done when:* run tests pass, including `MinInt64 / -1`, shifts ≥ the bit width, and out-of-range float → int.
- **P04.05 Lower strings/slices/refs/maps**: maps use a temporary runtime until P05.06. *Done when:* run tests pass.
- **P04.06 Lower methods & interfaces**: static itabs, borrowed interfaces, shared interface values, type switches, assertions. *Done when:* run tests pass.
- **P04.07 Lower closures, defer, panics**:
  - closures with shared cells for captured variables;
  - defer-extended locals: per-execution storage, the defer record owns the local, the block-end drop is skipped when the record exists, and it is dropped after all deferred calls (P01 D1.9);
  - escape analysis decides cells vs. plain references, using the "callee keeps this parameter" bits from export data (P01 D1.11);
  - panics as native C++ exceptions, with catch-all trampolines (D4.6);
  - starts after the P04.15 spike.

  *Done when:* run tests pass, including nested panics and a closure passed to a function that stores it (the `http.HandleFunc` pattern).
- **P04.08 Monomorphization**: instantiate generics while lowering to GIR; in source mode, each instance is emitted once per build; release builds emit `linkonce_odr` + COMDAT (D4.3). Cached homes across packages are P06.06. *Done when:* generic run tests pass, and duplicates fold in release.
- **P04.09 Flow-sensitive ownership**: use-after-move, defer/goroutine borrow checks, drop elaboration on normal and unwind paths; `gopp check` now runs these too. *Done when:* the ownership suite passes under ASan.
- **P04.10 `!` lowering**: on a cold path, return the error (functions that can fail) or panic with `errors.MustError` (elsewhere). *Done when:* both paths and the message format are tested.
- **P04.11 C ABI via Clang**: classification through Clang's `CodeGenABITypes`, plus Go++'s own lowering for every `ABIArgInfo` kind (sized L). *Done when:* struct-by-value tests against Clang-built C pass on amd64 and arm64.
- **P04.12 DWARF**: lines, variables, types. *Done when:* gdb and lldb can break and print locals.
- **P04.13 Codegen pipelines per `opt`**: the `opt 0` (dev), `opt 1`, `opt 2`, `opt 3`, `opt s`, `opt z` pipelines, plus the `-pipeline=fastisel` baseline (D4.8, P06 D6.6). *Done when:* the `opt` setting selects the pipelines and tests pass.
- **P04.14 Differential tests vs Go**:
  - the harness compares stdout, normalized stderr and the exit code against Go 1.25;
  - a tagged "expected differences" list;
  - programs Go++ rejects are counted separately;
  - the corpus includes `$GOROOT/test` run tests;
  - Go's outputs are saved once as golden files (D4.10).

  *Done when:* M1 passes (≥ 100 programs match).
- **P04.15 EH cost spike**:
  - generate about 100k lines of Go++-shaped LLVM IR (invokes, cleanups, checks);
  - time `llc -O0` on x86-64;
  - count FastISel fallbacks;
  - choose: patch FastISel, use GlobalISel, or decide D6.9.

  *Done when:* an ADR with the numbers is merged, before P04.07 starts.
- **P04.16 Bootstrap mode** (D4.11): source mode (all packages from source); host EH runtime until P08.10; the runtime's C/asm parts built by CMake with `gopp`. *Done when:* a three-package program builds in source mode and its panic test passes.
- **P04.17 Symbol names** (D4.13): naming spec appendix, mangler and demangler. *Done when:* names are stable across packages and build orders, and the demangler round-trips them.
- **P04.18 Internal calling convention**: Go++ → Go++ calls return aggregates in registers; C-ABI thunks for callbacks and `@Export` (D4.4). *Done when:* `(T, error)` returns stay in registers (FileCheck), and C callbacks still work.
- **P04.19 Fast test runner** (D4.14): batch compiles in one process, ORC JIT runs in a pool of executor processes, and real binaries only for the linking and interop set. *Done when:* the M1 differential corpus runs in ≤ 30 s on the dev machine, and a crashing test doesn't stop the run.

## P05 — Runtime and memory ([README](05-runtime-and-memory/README.md))

- **P05.01 Runtime skeleton**: Go++ + C sources, a per-target static archive in the cache, also loadable by the JIT test runner (P04.19). *Done when:* it is linked into every binary.
- **P05.02 Startup & exit**:
  - args/env and init order;
  - the exit sequence (D1.18): stop goroutines (a no-op until P12.12), drop `main`'s locals, C `exit()`;
  - `os.Exit` and exit codes.

  *Done when:* tests pass.
- **P05.03 Allocators**: `mem.Allocator`, the default libc allocator, `mem.Arena`, allocator objects stored in slices and maps (P01 D1.22). *Done when:* tests pass under ASan.
- **P05.04 Owned strings**: literals (cap 0), NUL-terminated buffers (capacity len+1), concatenation, conversions, UTF-8 helpers. *Done when:* tests and microbenchmarks pass.
- **P05.05 Slices**: growth policy, append/copy/clear, element drops, in-place re-slicing (D1.20), the allocator word (D1.22). *Done when:* tests pass, and the append benchmark against `std::vector::push_back` is recorded.
- **P05.06 Maps**: Swiss table, seeded hashing, randomized iteration start, drops, ref-key lookups, the active-iterator count (a new-key insert during `range` always panics, D1.21), the allocator in the header (D1.22). *Done when:* tests pass, and the benchmark against absl::flat_hash_map is recorded.
- **P05.07 Pointer kinds**: `Unique[T]`, `Shared[T]` (single-allocation control block), `Weak[T]`, with `new_unique`, `new_shared`, `new_weak`. *Done when:* tests pass.
- **P05.08 Type descriptors & glue**:
  - drop/clone/hash/eq glue;
  - relocate glue and the "has a zero value" flag;
  - iterative drop for `Unique[Self]` chains;
  - `nounwind` drop glue that preserves errno;
  - field tables for types converted to an interface (D1.12).

  *Done when:* tests pass, including a 1M-node list dropped on a goroutine stack.
- **P05.09 Dynamic interface conversions**: runtime cache. *Done when:* tests pass.
- **P05.10 Panic runtime**:
  - the exception object and backtrace capture;
  - the compact runtime line table (D5.7);
  - messages, and Go-exact nested panics.

  *Done when:* golden-output tests pass, including nested panics and a stripped binary.
- **P05.11 Safety checks**: bounds, nil, divide by zero, shifts, `@NoBounds`. *Done when:* tests pass.
- **P05.12 Sanitizers**:
  - `-asan` (+LSan), `-ubsan`, `-msan`;
  - runtime and std built per sanitizer;
  - compiler-rt per target.

  Goroutine support (ASan fibers, LSan stack roots, TSan and `-race`) is P12.09; libc++ per sanitizer is P08.10. *Done when:* seeded bugs are detected in single-goroutine programs, and there are no false reports on the samples.
- **P05.13 M2 suite**: the samples without concurrency, C/C++ imports or macros run leak-free under ASan/LSan in CI. *Done when:* the job is green.
- **P05.14 Errors: shared values and traces** (D1.19, D5.11): static package-level errors, reference-counted error boxes, and the per-goroutine trace ring. *Done when:*
  - `err == io.EOF` works through three `!` hops;
  - the chain prints all three sites;
  - there is no allocation per hop.
- **P05.15 Bootstrap std** (D5.12): minimal `fmt.Println`/`Printf`, `os.Args`/`Open`/`Exit`, `io.ReadAll`, `errors.New`, `errors.MustError`. *Done when:* the P04.14 differential corpus prints through it.
- **P05.16 Spec-revision window**: after M2, check the ownership rules against the M2 programs, and update P01 through ADRs. *Done when:* the review notes are resolved.

## P06 — Build driver and compile speed ([README](06-build-driver-and-compile-speed/README.md))

- **P06.01 Compile benchmark & calibration**:
  - a generator for equivalent Go/Go++ projects, plus generic-heavy and C++-heavy scenarios (the C++-heavy one is measured once P08 lands, P08.13);
  - measure `go build` under the D6.8 conditions;
  - finalize the D6.8 numbers.

  *Done when:* the numbers are recorded in `bench/compile/README.md`.
- **P06.02 Export data `.gopx`**: sections with separate hashes (type API, bodies, positions), lazy index, versioning, C ABI signatures, foreign header recipes (D6.2). *Done when:*
  - the importer loads dependencies only from `.gopx`;
  - a one-line edit that adds no API doesn't change the API hash.
- **P06.03 Build graph & scheduler**: critical-path priority, thread pool, one LLVMContext and TargetMachine per thread, one Clang worker per package (D6.4). *Done when:* multi-package builds work, and two runs give identical outputs.
- **P06.04 Content-addressed cache**: action IDs (including header dependency lists), outputs, trimming, `gopp clean -cache`; the module cache is kept separate. *Done when:* a no-op rebuild is all cache hits.
- **P06.05 Pipelining**: dependents start once export data exists. *Done when:* a trace shows the overlap.
- **P06.06 Generic instance homes**: one home per instance at `opt 0` and `opt 1`, keyed by a full fingerprint (D4.3). *Done when:* each repeated instance is compiled once (trace counter), and a layout change in a type argument invalidates it.
- **P06.07 Parallel codegen**: module splitting at `opt 0` and `opt 1`; the partition count depends on code size. *Done when:* the speedup is recorded, and output is identical for `-p 1` and `-p 8`.
- **P06.08 Dev pipeline tuning**. *Done when:* the D6.6 targets are met or an ADR documents the gap.
- **P06.09 `-trace`**: Chrome trace including LLVM pass timings. *Done when:* traces load in Perfetto.
- **P06.10 CLI parity**: `gopp build/run/test/clean/env` flags and behavior. *Done when:* CLI tests pass.
- **P06.11 Budget gate in CI**. *Done when:* CI fails on a > 10% regression.
- **P06.12 Verification model** (D6.10): verify on download/extract; per-build presence checks only; `gopp mod verify`; file-hash cache. *Done when:* presence checks and the file-hash cache are unit-tested, and a no-op build of a 10k-file project stays within the no-op budget. The end-to-end checks with a large cdep and `gopp mod verify` are in P11.14 and P10.08.
- **P06.13 LSP overlays** (D6.11): unsaved editor buffers and a check-session API. *Done when:* an edit in an unsaved buffer of one package changes diagnostics in a dependent package.
- **P06.14 Build profiles in the driver** (D6.12): the nine settings with the `debug` and `release` defaults; `inherits`, `deps` rows and the precedence order; only the main module's profiles; effective settings in every action ID; `opt` selects the codegen pipeline; `panic`, `strip`, `lto`, `pgo`, `cpu` and `link` reach codegen and the linker; profile terms in `//gopp:build`. *Done when:* tests show that `debug` and `release` of one project give different action IDs, that two profiles with equal settings share cache entries, that `deps ( * opt 2 )` changes only dependencies, and that `//gopp:build debug` selects a file only in `debug`.
- **P06.15 Profile CLI flags** (D6.12): `-release`, `-profile=NAME`, the nine one-off overrides, and `-race`/`-asan`/`-ubsan`/`-msan` as `-sanitize` shorthands, on `build`, `run`, `test` and `check`. *Done when:* CLI tests cover each flag, the conflict errors and the unknown-profile error, and `-opt=0` overrides a `deps` row.
- **P06.16 `target/` layout** (D6.12): `target/<profile>/` with the binary, `tests/` and `cdeps/`; `-target-dir`, `GOPPTARGETDIR`, `gopp env`, `gopp clean [-profile=]`; the cache stays in `$GOPPCACHE`. *Done when:* debug and release builds of one project sit side by side, deleting `target/` and rebuilding is all cache hits, and a second worktree builds from the same cache.

## P07 — C interop ([README](07-c-interop/README.md))

- **P07.01 Clang workers**: one CompilerInstance per package on one thread; PCH sharing between packages with equal headers and flags; header search order; source-ordered header imports. The ABI part moved to P04.11/P04.16. *Done when:* `<stdio.h>` imports, and two packages share one PCH.
- **P07.02 Lazy translation & header packages**:
  - one package per header;
  - membership by declaration home (D7.3);
  - identity across units = USR + layout hash, with a mismatch error (D7.2).

  *Done when:* `stdio.printf` and `zlib.crc32` resolve, and a layout mismatch between two packages is reported.
- **P07.03 Type mapping**: per D7.4, including ASTRecordLayout, unions, bitfields, enums, the typedef rule for `size_t`, the reverse mapping and the `ffi` spellings for C++. *Done when:* layout tests against Clang `sizeof`/`offsetof` pass.
- **P07.04 Calls**: ABI lowering, varargs, cgo-style errno (`n, err := …` and `!`). *Done when:* tests pass.
- **P07.05 C macros**: untyped numeric constants, alias macros, simple function-like macros with inferred parameter types. *Done when:* tests on `limits.h`, `zlib.h` (with and without `Z_PREFIX`) and SDL2 constants pass.
- **P07.06 Header inline functions**: Clang CodeGen into the package module. *Done when:* they are inlined in release builds (FileCheck).
- **P07.07 Strings & memory helpers**:
  - owned strings pass to C with no copy, refs as temporaries;
  - empty slices pass non-NULL;
  - inner-NUL check;
  - variadic rules;
  - `std::string_view` mapping;
  - `ffi.String`, `ffi.CString`;
  - the vet list of pointer-keeping functions.

  *Done when:* tests pass, including zlib `crc32` over empty chunks.
- **P07.08 Callbacks**:
  - Go++ functions passed as C function pointers (`ffi.Func[...]`), qsort;
  - a panic in a callback exits with code 2 by default, and `@Unwind` opts in to unwinding through C.

  Foreign threads and pinning are P12.08. *Done when:* tests pass, including a panic inside a qsort callback, with and without `@Unwind`.
- **P07.09 Package-local C/C++/asm and own-module headers**: the module's `cflags` and `ldflags` directives from gopp.mod (D1.15, P10.01); header imports from your own and dependency modules (D7.3). *Done when:* the sqlite amalgamation sample builds, with `sqlite3.h` imported from the package.
- **P07.10 `gopp cc` / `c++` / `ar`**. *Done when:* they build a small C project.
- **P07.11 Import unit caching**: PCH + binding export data. *Done when:* warm import overhead is ≤ 20 ms per package.
- **P07.12 ABI fuzzing**: C signatures in both directions, amd64 and arm64. *Done when:* ≥ 10k signatures per target run with 0 mismatches.
- **P07.13 Export to C**: `@Export("C")`, c-archive/c-shared, header generation. *Done when:* a C program links a Go++ library.
- **P07.14 musl cross builds**. *Done when:* a linux/arm64 binary built on amd64 runs under QEMU.
- **P07.15 Header torture tests**: all public libc headers, plus zlib, sqlite3, SDL2 and libcurl. *Done when:* each imports without errors, and a sample of declarations is checked against Clang.
- **P07.16 Struct tags & transparent unions** (D7.3). *Done when:* `stat.stat` (function) and `stat.struct_stat` (type) both work, and `socket.bind` accepts a `*sockaddr_in`.
- **P07.17 Decide P07 Q1–Q3**: ADRs. *Done when:* merged.

## P08 — C++ interop ([README](08-cpp-interop/README.md))

- **P08.01 C++ import units & header packages**: namespace flattening from the root namespace, collision errors, recorded resolutions, standard flags. *Done when:*
  - `vector.vector[int32]`, `algorithm.sort` and `vector.pmr.vector` resolve;
  - `cmath.abs(-2.5)` returns 2.5 with both libc++ and libstdc++.
- **P08.02 Stage 1: free functions & POD**: enums, overloads via Sema, the allowed implicit conversions (D8.4). *Done when:* tests pass, including a rejected narrowing conversion.
- **P08.03 Stage 2: classes & lifetimes**:
  - methods, constructors, destructors;
  - `Clone`, moves, relocation (D8.3);
  - the no-zero-value rule.

  *Done when:*
  - `std::string` (libc++ and libstdc++) and `std::unique_ptr` tests pass;
  - a growing `[]` of `std::string` stays valid.
- **P08.04 Same-module C++ codegen**. *Done when:* the inlining FileCheck test passes.
- **P08.05 Stage 3: templates**: class and function templates via Sema. *Done when:* vector/map/sort tests pass.
- **P08.06 Stage 4: operators & iterators**: including `range` over `begin()`/`end()`: the `&x` form and the one-variable form (index for indexable containers, key for associative ones, error for `std::list`). *Done when:* tests pass.
- **P08.07 References**: `T&`, `const T&`, `T&&`. *Done when:* tests pass.
- **P08.08 Inheritance & virtuals**. *Done when:* tests pass.
- **P08.09 Stage 5: exceptions ↔ panics**: native-exception panics (D4.6), `ffi.Exception`, `ffi.Try`, forced unwinding left alone. *Done when:* both directions work, and destructors are verified to run on both sides.
- **P08.10 C++ stdlib builds**: libc++/libc++abi/libunwind per target and per sanitizer (P05 D5.9); libstdc++ mode with libgcc_s; one unwinder per binary. *Done when:* both modes link a sample.
- **P08.11 Unsupported-feature diagnostics**: the D8.12 list. *Done when:* each item has a test.
- **P08.12 Interop samples**: a header-only template library, and fmt's runtime API built from package-local C++ files (P07 D7.9). *Done when:* the samples run.
- **P08.13 Performance parity**: vector loop vs clang++; the C++-heavy compile budget from P06 D6.8. *Done when:* the loop is within 5%, and the compile budget is set and met.
- **P08.14 C++ ABI fuzzing**: classes passed by value (trivial and non-trivial for calls), the order of `this`/`sret`, member calls. *Done when:* ≥ 5k generated signatures run with 0 mismatches on amd64 and arm64.
- **P08.15 Decide P08 Q3–Q4**: ADRs. *Done when:* merged.

## P09 — Macros ([README](09-macros/README.md))

- **P09.01 Macro design doc**: `docs/design/macros.md`. *Done when:* the owner approves it and all review comments are resolved.
- **P09.02 Declarative macros**: expansion + hygiene. *Done when:* the hygiene suite passes.
- **P09.03 GIR interpreter**: bytecode VM, fuel and memory limits, VM built-ins, the per-package comptime GIR artifact (D9.4, D9.5). *Done when:* GIR test programs run, and an `@Extern` call inside a macro gives a clear error.
- **P09.04 `gopp/macro` API**: syntax types, quoting, diagnostics, type queries, naming across packages. *Done when:* the API doc and tests exist.
- **P09.05 Procedural expression/statement macros**: including `macro func Name!` errors, and panics turned into compile errors (D9.11). *Done when:* tests pass, including the macro backtrace.
- **P09.06 Attached macros**: declared names, type queries, the D9.3 ordering rule; compile-time arguments are required only for `lit` parameters and `@` arguments (MAC-14). *Done when:* tests pass, including rejected cycles and constraint checks that see the generated methods.
- **P09.07 Built-in macros**: `#Assert` (no `fmt` needed), `#Stringify`, `#File`/`#Line`, `#Embed` (`ref string`), `#Todo`, `#Unreachable`. *Done when:* tests pass.
- **P09.08 `@Derive` set**: `JSON` (generated `MarshalJSON`/`UnmarshalJSON`) and `Equal` (for structs `==` can't compare). *Done when:* a P09 test package uses both. The `encoding/json` comparison is in P13.08.
- **P09.09 Expansion cache**: keyed by all the GIR an expansion executed. *Done when:* ≥ 95% hits on a no-op rebuild, and editing a helper the macro calls invalidates it.
- **P09.10 Tooling hooks**: `gopp expand`, macro backtraces, debug-info mapping. *Done when:* tests pass.
- **P09.11 Decide Q2–Q4**: ADRs. *Done when:* merged.
- **P09.12 Overhead budget**. *Done when:* the D9.10 budget is met or an ADR documents the gap.
- **P09.13 Profile macros and `@If`** (D9.12): `#Profile()` and `#Setting("name")` as string constants (the main module's profile name everywhere; per-package settings); `@If(cond)` on declarations; constant-condition `if` branches dropped from codegen but type-checked; the profile read recorded in the expansion key. *Done when:* tests show that a dead branch and an `@If(false)` function are absent from the object file but still produce type errors, that a live use of a disabled declaration is an error, and that switching profile re-expands only packages that read it.

## P10 — Modules: gopp.mod / gopp.sum ([README](10-modules-gopp-mod-sum/README.md))

- **P10.01 gopp.mod parser/printer**:
  - the go.mod grammar plus `gopp`, `toolchain`, `cxxstdlib`, `csystem`, and the module-wide `cxxstd`, `cflags` and `ldflags` (D10.2);
  - `cdep` blocks with `uses`, `tag` and `// indirect`;
  - classification by the longest prefix (D10.1).

  *Done when:* round-trip and canonical-format tests pass.
- **P10.02 gopp.sum**: `h1:` dirhash, `/cdep`, `/archive` and `/notfound` lines, verification per P06 D6.10. *Done when:* tests pass, including Go's dirhash vectors.
- **P10.03 Versions**: semver, pseudo-versions, `/vN` rules. *Done when:* tests pass.
- **P10.04 MVS**: including replace/exclude/retract, and the cdep merge rules (D10.3). *Done when:* ported Go MVS cases pass, and conflicting cdep identity keys are reported.
- **P10.05 Fetchers**: git (tag → commit, submodules), HTTPS, proxy client, auth. *Done when:* integration tests pass against a local git server and a static proxy.
- **P10.06 Module cache**: read-only trees and archives, cross-process locking, a separate directory, `gopp clean -modcache`. *Done when:* tests pass, and `gopp clean -cache` leaves the module cache untouched.
- **P10.07 Commands**: `mod init/tidy/download/verify/graph/why/edit/vendor`, `get`, `list -m`. `tidy` writes `// indirect` cdeps. *Done when:* CLI tests pass.
- **P10.08 Tamper & offline tests**. *Done when:* a moved tag, an edited cache, a missing sum and no network are all handled.
- **P10.09 Modules reference doc**. *Done when:* `docs/modules.md` is merged.
- **P10.10 cdep tree format** (D10.10): symlinks, exec bits, submodules, size limit, tar proxy format, URL conflicts. *Done when:* hashing a repo with symlinks and submodules is stable across machines.
- **P10.11 cdep versions** (D10.4): the `tag` key or pattern, pseudo-versions for untagged repos, explicit pins for different majors. *Done when:* fmt `12.2.0`, `openssl-3.3.1` and untagged stb all resolve.
- **P10.12 Build profile blocks** (D10.11): the `profile` block grammar (`inherits`, settings, `deps ( … )`), the `profile ( … )` pin inside `cdep` blocks, validation and canonical printing; ignored in dependency modules. *Done when:* round-trip and canonical-format tests pass, errors are tested (cycle, bad value, unknown `deps` path, `deps` row with a whole-program setting, profile name equal to `linux`), and a `profile` block in a dependency's gopp.mod changes nothing.

## P11 — C/C++ deps from the internet + CMake ([README](11-c-cpp-deps-and-cmake/README.md))

- **P11.01 cdep schema**: keys `commit`, `url`, `mode`, `cmake`, `provides`, `uses`, `tag`, `option`, `patch`, and validation. *Done when:* tests pass.
- **P11.02 Fetch & lock**: commit pin, archive `sha256:`, tree `h1:`, the version → tag mapping; `gopp get -c`. *Done when:* it writes gopp.mod + gopp.sum.
- **P11.03 Local CMake files**: the `cmake ./path.cmake` key, the generated wrapper project, `GOPP_CDEP_SOURCE_DIR`, `include()`d files hashed into the key. *Done when:* Dear ImGui (git) and the sqlite amalgamation (archive) build from local CMake files.
- **P11.04 CMake & Ninja provisioning**: pinned, verified, cached; the pinned CMake version is the minimum for `GOPPCMAKE`. *Done when:* it works on a clean machine.
- **P11.05 Toolchain file**. *Done when:* zlib builds with `gopp cc`. The profile part is P11.19.
- **P11.06 Dependency provider**:
  - system packages;
  - `csystem` mappings;
  - optional "not found" answers recorded in gopp.sum;
  - the satisfy order (own config → Find module → generated config);
  - host isolation (find-root modes, `PKG_CONFIG_LIBDIR`, clean environment).

  *Done when:*
  - libpng finds the gopp-built zlib;
  - GLFW configures with Threads and `csystem` X11;
  - a host-only library is not found.
- **P11.07 Sandbox**: Landlock read/write allow-lists, plus a mount namespace when available; staging install dir; a fallback warning. *Done when:* the malicious fixture's network access, out-of-tree writes and `~/.ssh` read are all blocked.
- **P11.08 Probe project & File API**: generate the consumer probe project; read its `compileGroups` and `link.commandFragments`. *Done when:*
  - fmt's usage flags exclude `FMT_LIB_EXPORT`;
  - zlib's link line is complete.
- **P11.09 Import → targets & link set**:
  - imports resolve in the installed include tree;
  - generated headers are allowed;
  - the target-choice rule, with ambiguity errors.

  *Done when:*
  - zlib links `zlibstatic` with `mode static` and `zlib` with `mode dynamic`;
  - OpenSSL's generated `ssl.h` imports;
  - the ambiguity error is tested.
- **P11.10 Cache keys & reuse**: the full key from D11.5 (tools, sysroot, environment, edges), DESTDIR staging, reproducibility flags. *Done when:* a cross-project cache hit is verified, and changing the CMake version misses the cache.
- **P11.11 Version uniqueness**: one version per library; duplicate-symbol scan before linking; bundled-copy detection. *Done when:* tests pass, including OpenCV-style bundled zlib next to a zlib cdep.
- **P11.12 Cross-compilation**: host builds for generator tools; `CMAKE_CROSSCOMPILING_EMULATOR` for `try_run`. *Done when:* the matrix runs on arm64 under QEMU.
- **P11.13 Commands**: `gopp cdep targets|build`, discovery mode, vendoring with cdeps (patch and CMake files included). *Done when:* CLI tests pass.
- **P11.14 M7 matrix in CI**. *Done when:* green, and a no-op build with the matrix's largest cdep stays within the P06 no-op budget (P06.12).
- **P11.15 `mode dynamic` runtime**: copy shared libraries next to the binary under their SONAME; RPATH `$ORIGIN`; the rule for mixed chains. *Done when:* a `mode dynamic` libpng + zlib sample runs from its output directory on Linux.
- **P11.16 `csystem`**: pkg-config lookup, link flags, the headers each entry owns, `find_package` mappings (D11.11). *Done when:* a sample links a system library through `csystem`, and its `"<…>"` import links it.
- **P11.17 `-hermetic`**: reject `csystem` entries and builds without Landlock. *Done when:* tests show both are rejected.
- **P11.18 Transitive cdeps** (D11.10): `uses` edges, recorded by `get -c`/`tidy` and checked at configure; build order and cache keys from edges. *Done when:* a cached libpng + zlib rebuild runs no CMake, and an unrecorded `find_package` edge fails with a clear error.
- **P11.19 Profile in the toolchain file** (D11.13): `GOPP_PROFILE`, the `CMAKE_BUILD_TYPE` table, the opt/debuginfo/cpu/pgo/sanitize flags, `CMAKE_INTERPROCEDURAL_OPTIMIZATION` for LTO, `BUILD_SHARED_LIBS` from `link` unless `mode` is set, C++ exceptions left on. *Done when:* a probe CMake project prints the expected variables and flags for `debug`, `release` and a custom profile, and zlib built with `lto thin` links into a Go++ binary.
- **P11.20 Profile in cdep keys, pins and build trees** (D11.13): the profile name and the effective settings in the cdep cache key; the `profile ( opt 2 )` pin and its precedence; build trees in `target/<profile>/cdeps/`. *Done when:* `debug` and `release` builds of zlib are two cache entries, a pin changes only that cdep, a second worktree gets a cache hit with no CMake run, and moving the build tree does not change the output hash.

## P12 — Concurrency ([README](12-concurrency/README.md))

- **P12.01 Context switch**: amd64 SysV and arm64 assembly plus a coroutine primitive; switches save the C++ exception state (D12.4). *Done when:* the ping-pong microbenchmark runs, and a deferred call that blocks during a panic resumes on another thread with the panic intact.
- **P12.02 Stack strategy ADR**: guard pages (with stack probes) vs prologue checks, sizes, VMA limits. *Done when:* the ADR is merged.
- **P12.03 Scheduler**: G/M/P, work stealing, sysmon handoff, preemption flags checked in prologues and on loop back-edges. *Done when:* stress tests pass, and a spin loop on an atomic flag no longer hangs with one P.
- **P12.04 `go` lowering**: argument moves and copies, closures with shared cells, the error for pointers into locals. *Done when:* tests pass, including the classic worker pool written as in Go.
- **P12.05 Channels & select**: send cases move only when chosen. *Done when:* semantics tests derived from Go pass.
- **P12.06 sync & atomics**: parking via futex; `sync.Scope`; `@Pinned` sync types. *Done when:* tests pass, and the contention benchmark is recorded.
- **P12.07 Netpoller**: epoll. *Done when:* the echo server sample works.
- **P12.08 Threads, TLS and C calls**:
  - `LockOSThread` and the blocking-call handoff;
  - thread-local barriers at yield points;
  - pinning while C frames are on the stack;
  - adopting foreign threads.

  *Done when:* tests pass, including errno read after a yield and a callback from a thread created in C.
- **P12.09 Sanitizers with goroutines**: ASan fibers, LSan stack roots, TSan `no_sync` fiber switches, `-race` (P05 D5.9). *Done when:* seeded races and goroutine-stack bugs are caught, and the samples are clean.
- **P12.10 Benchmarks vs Go**. *Done when:* numbers are recorded against the D12.11 targets.
- **P12.11 Timers**: `time.After`, `time.Ticker`, `select` timeouts integrated with the netpoller. *Done when:* the timer tests pass with all Ps busy.
- **P12.12 Stop at exit** (D12.12): `main` returning, `os.Exit` and unrecovered panics stop the other goroutines before C `exit()`. *Done when:* a program whose `main` returns while 1,000 goroutines run (some inside C calls) exits cleanly, and its atexit handlers and C++ static destructors run with no Go++ code running.

## P13 — Standard library v1 ([README](13-standard-library/README.md))

- **P13.01 API conventions doc**: ownership in signatures, `!` functions, shared errors, `move dst []T`. *Done when:* `docs/std-conventions.md` is merged.
- **P13.02 T1 core**: errors (shared values, `MustError`), unsafe, runtime, mem, math, math/bits, unicode/utf8. *Done when:* tests pass.
- **P13.03 T1 text**: strings, bytes, strconv. *Done when:* ported tests pass.
- **P13.04 T1 fmt**: Go-compatible `%v`/`%+v` on any type via field tables (D13.4); replaces the bootstrap `fmt`. *Done when:* output matches Go's `fmt` on the ported test cases.
- **P13.05 T1 collections**: slices, maps, sort, `container/deque`. *Done when:* tests pass.
- **P13.06 T1 I/O & OS**: io, bufio, os, path, path/filepath, flag. *Done when:* tests pass.
- **P13.07 T1 time & testing**: including Q3 (tz data). *Done when:* tests pass.
- **P13.08 T2 encoding, hash, crypto**: includes `encoding/json`. *Done when:* tests pass, and `encoding/json` output is identical with and without `@Derive(JSON)`.
- **P13.09 T2 context, log, exec, signal, net**. *Done when:* tests pass.
- **P13.10 T2 regexp, compress, container**. *Done when:* tests pass.
- **P13.11 First `x/` interop packages**. *Done when:* the cdep-backed `x/zstd` round-trips its test files.
- **P13.12 Examples**. *Done when:* every std package has a runnable example, checked by `gopp test`.
- **P13.13 Decide P13 Q2–Q3**: `sync.Pool`, time zone data. *Done when:* ADRs merged.

## P14 — Tooling ([README](14-tooling/README.md))

- **P14.01 CLI consolidation**: help text, `gopp env`, `gopp tool`, every subcommand in D14.1 (including `parse`, `cdep`, `list`, `fix`, `new`). *Done when:* CLI tests cover every subcommand's help.
- **P14.02 `gopp test`**: runner, parallelism, `-run`/`-count`, examples. *Done when:* the std tests run through it.
- **P14.03 Bench & fuzz**: libFuzzer integration. *Done when:* fuzzing finds a seeded bug.
- **P14.04 Coverage**: LLVM source-based coverage, HTML report. *Done when:* a report is generated.
- **P14.05 LSP core**:
  - diagnostics, hover, definition, completion, formatting;
  - overlays (P06.13);
  - Clang in a worker process;
  - per-snapshot offsets.

  *Done when:* VS Code works on the samples, and completion p95 is < 100 ms on a 100k-line workspace.
- **P14.06 LSP advanced**: references, rename, code actions, expansion view, ownership hints, go-to-definition into C/C++. *Done when:* tests pass.
- **P14.07 Debugger support**: LLDB/GDB pretty printers, goroutine listing. *Done when:* batch-mode tests pass.
- **P14.08 Editors**: VS Code extension, tree-sitter grammar, configs for Neovim, Helix and Zed. *Done when:* the extension package builds, and the tree-sitter grammar parses the parser corpus.
- **P14.09 `gopp doc`**: terminal + HTML. *Done when:* the std docs render.
- **P14.10 Distribution & `toolchain`**: archives with prebuilt tier-1 toolchain parts, version switching. *Done when:* switching versions works.
- **P14.11 `gopp new`** (D14.1): creates a project directory with `gopp.mod` (like `gopp mod init`), a hello-world `main.gopp` and a `.gitignore` containing `target/` (P06 D6.12). *Done when:* a CLI test runs `gopp new demo && cd demo && gopp build` and finds `target/` ignored by `git status`.

## P15 — Performance ([README](15-performance/README.md))

- **P15.01 Benchmark suite**: Go++/C++/Go/Rust implementations plus a harness. *Done when:* all run with verified outputs.
- **P15.02 Perf CI & dashboard**: nightly runs, and alerts on a > 3% regression. *Done when:* nightly results are published, and a seeded regression raises an alert.
- **P15.03 Allocation elision**: allocator attributes, heap-to-stack. *Done when:* a microbenchmark shows elision.
- **P15.04 Ownership-derived attributes**: the rule table from D15.3 (nonnull, dereferenceable, noundef, readonly, limited captures; no `noalias` on `Unique[T]`). *Done when:* each attribute has a FileCheck test and a miscompile test, and the benchmark delta is recorded.
- **P15.05 Check elimination**: bounds and nil. *Done when:* the reduction is measured and recorded.
- **P15.06 Devirtualization**: at GIR level and via LTO WPD. *Done when:* tests pass.
- **P15.07 Cross-package inlining**: via export data. *Done when:* ≥ 5% geomean gain on the suite, or an ADR explains the gap.
- **P15.08 ThinLTO including cdeps**. *Done when:* the matrix builds with ThinLTO, and the gains are recorded.
- **P15.09 PGO**. *Done when:* ≥ 5% gain on the suite, or an ADR explains the gap.
- **P15.10 Allocator evaluation**: mimalloc vs libc → ADR (Q2). *Done when:* the ADR is merged.
- **P15.11 Binary size**: including the runtime line table. *Done when:* the D15.2 size target is met or an ADR documents the gap.
- **P15.12 Targets review**. *Done when:* a report against D15.2 exists.
- **P15.13 Padding hint**: `gopp vet` suggests a field order that wastes less padding. *Done when:* tests pass.
- **P15.14 Decide P15 Q1**: small-string optimization. *Done when:* the ADR is merged.

## P16 — Release ([README](16-release/README.md))

- **P16.01 v0.1 scope & checklist**. *Done when:* `RELEASE.md` exists.
- **P16.02 Trademark check** (D16.6): confirm "Go++" can be used publicly, or pick a new name. *Done when:* the ADR is merged.
- **P16.03 Reproducible signed builds + SBOM**. *Done when:*
  - two independent builds are bit-identical;
  - signatures verify;
  - the SBOM lists every bundled part.
- **P16.04 Docs**: tour, Go → Go++, C++ → Go++, interop cookbook, spec. *Done when:* the owner approves them.
- **P16.05 Installers**: Linux (tier 1), plus the flake package for NixOS (D16.4). *Done when:* a clean-machine install plus the tutorial takes < 10 minutes.
- **P16.06 Community files**: CONTRIBUTING, CoC, issue templates, SECURITY. *Done when:* present.
- **P16.07 Tag v0.1**. *Done when:* the tag is pushed, and the tier-1 archives are published.
- **P16.08 Self-hosting research plan**. *Done when:* `docs/design/self-hosting.md` exists.
