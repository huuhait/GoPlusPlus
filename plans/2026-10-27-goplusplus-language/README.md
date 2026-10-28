# Go++ (GoPlusPlus): language plan

> Created 2026-10-27 · Progress: [STATE.md](STATE.md) · Work items: [TODO.md](TODO.md)

Go++ is a compiled systems language with **Go's syntax and tooling**, **no garbage collector**, **C++-class runtime speed**, **Go-class build speed**, **hygienic macros**, **`call!()` for "must"**, and **first-class reuse of the C/C++ ecosystem**. You can import C/C++ headers directly, pull C/C++ and CMake projects from the internet, and lock them in `gopp.mod`/`gopp.sum` the same way Go modules are locked. **LLVM** is the backend. **Clang** is embedded for C/C++ import.

---

## 1. Requirements and how this plan meets them

| # | Requirement | Answer in this plan | Phases |
|---|---|---|---|
| R1 | Syntax like Go | The Go grammar is the base, with a small set of deltas: the ownership word `move`, borrowed views `View[string]`/`View[[]T]` (ADR-0020), owning pointers `Unique[*T]`/`Shared[*T]`/`Weak[*T]`, pointers that are never nil with `Option[T]` for "maybe" (ADR-0018), `!` for errors (`func f!()`, `call!()`), `#Name(...)`, `@Name`, and header imports (`"github.com/madler/zlib/zlib.h"`, `"<vector>"`). `gopp fmt` is canonical, like gofmt. | P01, P02 |
| R2 | Macros | Hygienic declarative macros (`macro`), plus procedural macros written in Go++ and run at compile time by a sandboxed interpreter. Invoked as `#Name(...)`, attached as `@Name`. | P09 |
| R3 | `!` for Must | `func load!(…) T` declares a function that can fail; callers see `(T, error)`. `val := call!()` unwraps the error: inside a function that can fail it returns the error to the caller, elsewhere (e.g. `main`) it panics with the source location. Go's `val, err := call()` still works. | P01, P03, P04 |
| R4 | Compile fast like Go | Each package compiles separately, in parallel, from its dependencies' export data (no headers). The build is pipelined, outputs go in a content-addressed cache, and the debug pipeline is cheap. C/C++ imports are cached. | P06 (+P02–P04) |
| R5 | Fast like C++ (or almost) | Value types, no GC, monomorphized generics, and LLVM O2/O3. C/C++ code is generated into the *same* LLVM module, so it inlines. LTO and PGO are available. | P04, P08, P15 |
| R6 | No GC | Ownership with RAII destructors, moves, explicit `clone`, borrowed views and arenas. No tracing GC. Reference counting happens only on shared handles: `Shared`/`Weak`, channels, interface values (including errors) and func values. | P01, P05 |
| R7 | Import any C/C++ files and packages | Go-style header imports, used like Go packages: `import "github.com/madler/zlib/zlib.h"` → `zlib.crc32(…)`, `import "<vector>"` → `vector.vector[int32]`. No `C.` prefix and no C-specific types. Embedded Clang parses headers and declarations are translated lazily. Clang Sema handles overloads and templates, and Clang CodeGen emits inline code. | P07, P08, P11 |
| R8 | Import any CMake project | gopp runs CMake with its own toolchain file and intercepts `find_package`/`FetchContent` through a dependency provider. Usage flags and link lines come from a small probe project, read through the CMake File API. CMake is the only build system in v1: a package without CMake gets a small local CMake file (`cmake ./cmake/X.cmake`), and `mode static`/`mode dynamic` picks the library type. | P11 |
| R9 | `gopp.mod` / `gopp.sum` like Go | Go's module design (MVS, semantic import versioning, `h1:` hashes), extended with `cdep` blocks for C/C++. | P10 |
| R10 | Internet C/C++ deps locked in **both** mod and sum | `gopp.mod` pins identity: the exact version plus an immutable commit or URL. `gopp.sum` pins content: the source-tree hash plus the raw archive hash. Unlocked transitive fetches fail the build. | P10, P11 |
| R11 | LLVM | LLVM is the only production backend. It also gives us LLD, sanitizers, coverage, PGO, LTO, ORC JIT, DWARF and many targets. | P04 (+all) |

## 2. Principles, in priority order (used to break ties)

1. **Predictable performance.** No GC. Reference counting only on shared handles (`Shared`/`Weak`, channels, interfaces, func values). Allocation happens only in these places:
   - `new_unique`, `new_shared`, `make`, `append`, string `+`, `clone`;
   - conversions to owned values (`string(v)`);
   - putting a value into an interface variable, or a callee keeping a borrowed interface argument (P01 D1.12);
   - variables captured by a closure that outlives its function;
   - goroutine stacks and map growth;
   - the key clone when a new entry is inserted into a map (any owning key: a view key or a borrowed one; P01 D1.6, OWN-V4a);
   - defer records for deferred calls in loops (they hold the extended locals, D1.9);
   - passing a `View[string]` or a borrowed `string` parameter to C (a temporary copy, on the stack when short).
2. **Reuse, don't rewrite.** C and C++ libraries are first-class dependencies. When in doubt, Clang decides C/C++ semantics (layout, overloads, templates, ABI, mangling).
3. **Go's simplicity.** Where Go has a construct, keep its syntax and, wherever GC isn't involved, its semantics. Add new syntax only where ownership or interop requires it.
4. **Fast edit–build–run loop.** Debug builds favor compile speed and release builds favor runtime speed. Language semantics are identical in every profile; the one exception is `panic abort`, where `recover()` returns nil (P06 D6.12).
5. **Safe defaults when they are free.** Bounds checks, zero values, nil checks, defined integer overflow, and use-after-move errors, and a borrow check without lifetime annotations (ADR-0021). Sanitizers are one flag away for what is left undefined.
6. **Reproducible, locked builds.** Every byte that goes into a build comes from a hashed input.
7. **Fast loop for building Go++ itself.** Rebuilding `gopp` and running the tests for what you changed take seconds, not minutes, even as the code grows. A slow loop is a bug: fix it with narrower test targets, caches and tests that stop early, not by waiting (P00 D0.13).

## 3. Non-goals for v1

- Source compatibility with Go. Go code that relies on GC won't compile unchanged, and neither will Go code that uses nil pointers, nil funcs or nil interfaces with methods (they are never nil; "maybe" is `Option[T]`, ADR-0018 and its amendment; `error` and `any` keep `nil`); a migration helper comes after v1.
- Binary caches for C/C++ dependencies, signed or not (never, P11 Q2): cdeps are always built from source.
- Consuming Go modules. Importing a Go module is an error in v1; after v1.0 it needs a dual engine (Go semantics with GC next to Go++) (P10 Q2).
- Rust-style lifetime annotations, and full memory safety: `unsafe`, C and C++ interop, hidden aliases, data races and `longjmp` stay undefined. Borrows are checked by flow and liveness without annotations (ADR-0021).
- A tracing GC, or automatic refcounting of everything.
- User-defined operator overloading, class inheritance, or exceptions as an error mechanism. Imported C++ operators and exceptions *are* supported at the boundary.
- A user-facing `reflect` package. `fmt` and `encoding/json` still work on any type, using small field tables the compiler emits (P01 D1.12).
- C/C++ build systems other than CMake (Meson, Autotools, Bazel). A package without CMake gets a small local CMake file instead (P11); other build systems may come after v1.
- A non-LLVM production backend.
- A stable ABI between Go++ shared libraries.
- macOS and Windows in v0.1. v0.1 is Linux only (amd64, arm64); the others come after v0.1 (P16).

## 4. Go++ at a glance (illustrative)

```go
package main

import (
    "encoding/json"
    "fmt"
    "io"
    "os"

    "<algorithm>"                   // C++ standard library headers
    "<vector>"
    "github.com/madler/zlib/zlib.h" // C header from a locked C/C++ package (gopp.mod below)
)

@Derive(JSON)                    // optional: generates a fast MarshalJSON at compile time
type Point struct{ X, Y int }

type Doc struct {
    Title string                 // owned: freed when the Doc is destroyed
    Tags  []string               // owned growable array of owned strings
    Next  Unique[*Doc]            // owning pointer (a plain *Doc would not own)
}

// `!` after the name: load can fail, so callers get ([]byte, error).
// Parameters borrow by default; results are owned.
func load!(path string) []byte {
    f := os.Open!(path)          // on error, load returns it to its caller
    return io.ReadAll!(f)        // f is closed when it goes out of scope (RAII)
}

func checksum(data []byte) uint32 {
    #Assert(len(data) > 0, "empty input")             // declarative macro
    return uint32(zlib.crc32(0, data, uint32(len(data)))) // the slice passes as a C pointer
}

func main() {
    data := load!(os.Args[1])    // main can't return an error, so here `!` panics
    fmt.Println("crc32:", checksum(data))

    var v vector.vector[int32]   // a real std::vector<int32_t>; destroyed at scope end
    for _, b := range data[:min(8, len(data))] {      // slicing yields a view: no copy
        v.push_back(int32(b))
    }
    algorithm.sort(v.begin(), v.end())

    done := make(chan Point)
    go func() { done <- Point{1, 2} }()
    p := <-done
    fmt.Println(p)                        // {1 2}: printing works on any type, as in Go
    fmt.Println(string(json.Marshal!(p))) // {"X":1,"Y":2}, via the derived MarshalJSON
}
```

`gopp.mod` and `gopp.sum` for the program above (hashes elided):

```
module github.com/acme/crcdemo

gopp 0.1

cdep github.com/madler/zlib v1.3.1 (
    commit   <full commit hash that tag v1.3.1 resolved to>
    mode     static
    provides ZLIB
    option   ZLIB_BUILD_EXAMPLES=OFF
)
```

```
github.com/madler/zlib v1.3.1/cdep h1:<sha256 hash of the source tree, base64>=
```

## 5. Go vs Go++: the deltas

| Area | Go | Go++ |
|---|---|---|
| Memory | Tracing GC | Ownership + deterministic destructors (RAII); no GC |
| Allocators | One GC heap | Each owned slice and map stores its allocator, so it can live in an arena (`mem.Arena`) |
| `string`, `[]T`, `map` | GC-shared references | Owned values. Assignment moves them; `x.Clone()` copies |
| Interfaces, errors, func values, channels | GC-shared references | Shared handles: copies share the same value through a reference count, as in Go (`err == io.EOF` works) |
| Closures | Capture variables by reference | Same. A closure that outlives its function keeps captured variables in shared cells |
| Borrowing | Everything is shared | Parameters borrow by default; `move` before a parameter name takes ownership. `View[string]` / `View[[]T]` store or return borrows |
| `*T` | GC pointer | Raw non-owning pointer. `Unique[*T]` owns; `Shared[*T]` / `Weak[*T]` are refcounted, opt-in |
| `new(T)`, `&T{…}` | GC allocation | Removed. Allocation is explicit: `new_unique(v)` → `Unique[*T]`, `new_shared(v)` → `Shared[*T]`; `new_weak(s)` makes a `Weak[*T]` from a `Shared[*T]` |
| Errors | `val, err := call()` + `if err != nil` | Same, plus `func f!(…) T` (can fail; returns `(T, error)`) and `val := call!()`, which returns the error from functions that can fail and panics elsewhere |
| Metaprogramming | `go generate`, `reflect` | `macro`, `#Name(…)`, `@Name`; no `reflect` package (`fmt`/`json` read compiler-emitted field tables) |
| C interop | cgo (`C.` prefix, comment preamble, costly calls) | `import "github.com/madler/zlib/zlib.h"` or `"<stdio.h>"`, used as `zlib.crc32(…)`; plain Go++ types, zero-overhead calls, `gopp cc` |
| C++ interop | — | Same import style (`import "<vector>"` → `vector.vector[int32]`): classes, templates, RAII, exceptions |
| Panics | Go runtime unwinding | Platform unwinder shared with C++ exceptions; destructors run |
| Goroutines | Growable moving stacks, async preemption | Fixed non-moving stacks (so C can run on them); cooperative scheduling with yield points in function prologues and loop back-edges |
| Generics | GC-shape stenciling + dictionaries | Full monomorphization with cross-package dedupe |
| Generic methods | Not allowed | Allowed; they can't satisfy interface methods |
| Data races | Memory model limits damage | Undefined behavior, detected with `-race` (TSan) |
| Toolchain | gc compiler + own linker | LLVM + embedded Clang + LLD in one `gopp` binary |
| Dependencies | `go.mod` / `go.sum` | `gopp.mod` / `gopp.sum` plus `cdep` blocks (commit in mod, hashes in sum) |

## 6. Architecture

```
.gopp sources                   C/C++ headers                  gopp.mod + gopp.sum
     │                                  │                              │
     ▼                                  ▼                              ▼
lexer → parser → AST            Clang importer                 module resolver (MVS)
     │                          (lazy decls, PCH cache)        fetch + verify (h1 / sha256)
     ▼                                  │                              │
macro expansion ◄─ GIR interpreter      │                      cdep builds: CMake (or a local CMake file)
     │                                  │                      sandboxed, compiled with `gopp cc`, cached
     ▼                                  │                              │
type checker ◄──────────────────────────┘                              │
     │                                                                 │
     ▼                                                                 │
GIR: ownership checks · drop elaboration · `!` · monomorphization      │
     │                                                                 │
     ▼                                                                 │
LLVM IR ◄── Clang CodeGen (C/C++ inline functions, template instances) │
     │                                                                 │
     ▼                                                                 │
LLVM passes → object files ──► LLD ◄── runtime + static C/C++ libs ◄───┘
                                │
                                ▼
                          native binary

Around all of it, the build driver: package DAG · export data (.gopx) · content-addressed cache · parallel jobs
```

- **GIR** is the Go++ mid-level IR: typed, CFG-based, place/rvalue form. It carries ownership checks, drop elaboration, monomorphization and cross-package inlining, and it is what the macro interpreter executes (P04, P09).
- **One binary.** `gopp` embeds LLVM, Clang and LLD and doubles as a C/C++ compiler (`gopp cc`, `gopp c++`). That keeps C/C++ dependencies ABI-consistent and simplifies cross-compilation (P07).

## 7. Phases

| Phase | Name | Size | Depends on | Milestone |
|---|---|---|---|---|
| [P00](00-foundations/README.md) | Foundations | S | — | M0 |
| [P01](01-language-spec/README.md) | Language specification v0 | L | P00 | M0, M2 |
| [P02](02-frontend/README.md) | Front-end: lexer, parser, AST, `gopp fmt` | M | P01 | M0 |
| [P03](03-semantic-analysis/README.md) | Semantic analysis | L | P02 | M0–M1 |
| [P04](04-gir-and-llvm-codegen/README.md) | GIR and LLVM codegen | XL | P03 | M0–M1 |
| [P05](05-runtime-and-memory/README.md) | Runtime and memory | L | P04 | M2 |
| [P06](06-build-driver-and-compile-speed/README.md) | Build driver and compile speed | L | P04, P05 | M3 |
| [P07](07-c-interop/README.md) | C interop | L | P04, P06 | M4 |
| [P08](08-cpp-interop/README.md) | C++ interop | XL | P05, P07 | M5 |
| [P09](09-macros/README.md) | Macros | L | P03, P04 | M6 |
| [P10](10-modules-gopp-mod-sum/README.md) | Modules: gopp.mod / gopp.sum | M | P06 | M7 |
| [P11](11-c-cpp-deps-and-cmake/README.md) | C/C++ deps from the internet + CMake | L | P07, P08, P10 | M7 |
| [P12](12-concurrency/README.md) | Concurrency | L | P05 | M8 |
| [P13](13-standard-library/README.md) | Standard library v1 | L | P05, P07, P09, P11, P12 | M9 |
| [P14](14-tooling/README.md) | Tooling | L | P03, P06, P13 | M9 |
| [P15](15-performance/README.md) | Performance | M | P04–P08, P11, P13 | M9 |
| [P16](16-release/README.md) | Release | M | all | M9 |

Size: S ≈ weeks, M ≈ 1–2 months, L ≈ 2–4 months, XL ≈ 4+ months for one focused engineer. These are relative sizes, not commitments.

**Milestones.** Each milestone is a vertical slice that runs end to end.

| Milestone | Name | Done when |
|---|---|---|
| M0 | Hello, LLVM | `gopp build` turns a one-file `package main` into a native binary through LLVM + LLD, for linux/amd64 and linux/arm64 (QEMU), on NixOS and on a standard (FHS) distro |
| M1 | Go core | The Go-like core subset passes ≥ 100 differential tests against `go run` |
| M2 | No-GC core | Ownership, destructors, `!`, panic/defer/recover work; samples run leak-free under ASan/LSan |
| M3 | Fast builds | Multi-package builds use export data and the cache; compile-speed budgets are enforced in CI |
| M4 | C bridge | C headers import, package-local C compiles, and `gopp cc` works |
| M5 | C++ bridge | `std::vector`/`string`/`map`, templates, RAII, and exceptions ↔ panics work |
| M6 | Macros | Declarative and procedural macros work, including `@Derive` |
| M7 | Packages | Go++ modules and internet C/C++/CMake deps are locked in gopp.mod + gopp.sum and rebuild offline |
| M8 | Concurrency | Goroutines, channels and select work and run TSan-clean |
| M9 | v0.1 alpha | Std v1 and tooling are done, performance targets are met (or each gap has an ADR), and v0.1 is released for Linux |

## 8. How to use this plan

- **`README.md`** (this file) holds the vision, requirements, principles, deltas and architecture.
- **`STATE.md`** tracks the status of the plan, each phase and each milestone.
- **`TODO.md`** lists every task by phase. Each task has a stable ID (`P03.07`) and a *done when* line. It defines the work and does not carry status.
- **`NN-phase/README.md`** says what the phase decides and builds: decisions with rationale and rejected alternatives, design, deliverables, exit criteria, risks and open questions.
- **`NN-phase/STATE.md`** holds the phase's task checklist and exit-criteria checklist (same IDs as TODO.md), an open-questions checklist, and notes and blockers. Some phases add extra checklists (P08 "Stages", P11 "Test matrix") with the same legend.

Status legend, used in every STATE.md: `[ ] not_started` · `[~] in_progress` · `[x] completed`.

Update rules:
1. When you start a task, mark it `[~]` in the phase STATE.md. If the phase is still `[ ]` in the root STATE.md, mark it `[~]` there too.
2. When the task is finished, mark it `[x]`. A phase is `[x]` only when **all its tasks, all its exit criteria and all its open questions** are `[x]`.
3. Whenever you change a phase STATE.md, also update its `Phase status` and `Last updated` lines.
4. When you discover new work, add it to TODO.md with the next free ID in that phase, then add it to the phase STATE.md.
5. Open questions are numbered (Q1, Q2, …) in the phase README and listed in the phase STATE.md. A question counts as `[x]` when it is decided, or deferred to a named phase.
6. Mark a milestone `[x]` in the root STATE.md when its "done when" (section 7) is met.
7. To change a decision, write an ADR in `docs/adr/` (process set up in P00). Then update the affected phase README and log the change in that phase's STATE.md, under "Decision changes".
8. Changes made **before the plan is accepted** are logged only in the root STATE.md log.

Every "Decision" in a phase README is the plan's current position. It stays in force until an ADR changes it.

**Dev loop:** keep the P00 D0.13 budgets. The working rules for agents are in the repo's `CLAUDE.md`.

## 9. Top risks

| Risk | Impact | Mitigation | Phase |
|---|---|---|---|
| C++ interop complexity (templates, exceptions, ABI) | Could stall M5 | Five shippable stages; Clang Sema/CodeGen own all C++ semantics; do C first | P08 |
| LLVM makes debug builds slow | Misses R4 | Shrink the work at GIR level, use O0 + FastISel, parallel codegen, pipelining and caching, with budgets measured from P06. Fallback: a baseline backend for debug builds only | P06 |
| Panic cleanup turns most calls into `invoke`, which x86-64 FastISel can't handle | Slow debug builds (R4) | A spike (P04.15) measures it before the panic design is fixed. Options: patch FastISel, use GlobalISel, or the D6.9 fallback | P04, P06 |
| Clang is single-threaded | Races, or all C++ work serialized | One Clang worker per package; packages share only PCH files (P07 D7.1) | P06, P07 |
| Ownership model feels un-Go-like | Adoption | Write the sample programs before implementing; keep a spec-revision window after M2 | P01, P05 |
| Dangling views/pointers | Memory bugs | Borrow checker without annotations (ADR-0021, P04.20), view types that make borrows visible, sanitizers behind one flag for the residue (`unsafe`, C interop, hidden aliases) | P01, P03, P05 |
| CMake builds run arbitrary code | Supply chain | Lock files, a Landlock sandbox (no network; writes only to a private source copy and the build dir), the dependency provider, and a hermetic mode | P11 |
| Bundling LLVM/Clang/LLD | Huge binary, slow CI | Prebuilt LLVM via Nix, ccache, shared-library dev builds | P00 |
| Scope (language + std + package manager + IDE) | Never ships | Milestones as vertical slices, a tiered std, strict non-goals | all |
| "Go" trademark | Forced rename | The owner keeps Go++; a trademark check runs before the first public release (P16.02). `gopp` avoids the clash with Linux's `gpp` tool | P00, P16 |
| LLVM API churn | Upgrade pain | One pinned LLVM major; LLVM/Clang calls confined to CodeGen/Import | P00 |
| Fixed goroutine stacks | Memory per goroutine; C stack overflow | Lazy commit, tunable sizes, guard pages or prologue checks | P12 |

## 10. Glossary

- **GIR**: Go++ IR, the typed CFG-based mid-level IR between the AST and LLVM IR.
- **Export data (`.gopx`)**: a binary summary of a compiled package (types, generic and inlinable bodies, macros). Importers read it instead of sources.
- **Owning type**: a type with a destructor, either directly (`drop`) or through its fields. It is move-only and copied with `clone`.
- **Trivial type**: a type copied bit-for-bit, with no destructor (numbers, bools, raw pointers, every `View[T]`, aggregates of these).
- **Shared handle**: a value whose copies all share one thing through a reference count: `Shared[*T]`, `Weak[*T]`, channels, interface values (including errors) and func values.
- **Drop glue**: compiler-generated code that destroys a value (it calls `drop`, then destroys the fields). **Drop elaboration** is the compiler step that inserts these calls.
- **Relocate**: moving a value to a new address (for example when a slice grows). It is `memcpy` for Go++ types, and the move constructor for C++ types that need one.
- **Itab**: the table of methods an interface value uses for one concrete type.
- **`linkonce_odr` / COMDAT**: LLVM's way to let identical copies of a function exist in many object files; the linker keeps one.
- **PCH**: a precompiled header, Clang's cached parse of a set of headers.
- **ODR**: C++'s "one definition rule": one function or type must have the same definition everywhere.
- **FastISel**: LLVM's fast, simple instruction selector for debug builds.
- **G/M/P and sysmon**: the goroutine scheduler's parts (goroutines, OS threads, processors) and its monitor thread, as in Go.
- **Hygiene**: names inside a macro can't clash with names at the place it is used.
- **Fuel**: an instruction budget for compile-time macro code, so a macro can't run forever.
- **Dirhash**: Go's SHA-256 hash of a whole file tree (`h1:`).
- **CMake File API**: CMake's machine-readable description of a configured project (targets, flags, link lines).
- **Probe project**: a tiny generated CMake project that links a cdep's targets, so gopp can read the exact flags and link line a user needs.
- **Landlock**: a Linux feature that limits which files a process may read and write, without needing root.
- **Must**: the `!` operator's job: take the value, or deal with the error (return it, or panic).
- **View**: a read-only borrow `View[T]`, deep, like Rust's `&T`. For a string or slice type (`View[string]`, `View[[]T]`, `View[Bytes]`) it is a `(ptr, len)` pair; for any other `T` one pointer.
- **cdep**: a C/C++ dependency declared in `gopp.mod`.
- **Header package**: an imported C/C++ header, used like a Go package named after the file (`zlib.crc32`, `vector.vector[int32]`).
- **`!` function**: a function declared `func f!(…) T`. It returns `(T, error)`, and `call!()` inside it returns errors to its caller instead of panicking. A Go-style function whose last result is `error` behaves the same way.
- **Import unit**: one Clang parse of a set of headers under one configuration (target, language, flags). Each package has its own; packages with the same headers and flags share the cached PCH file.
- **MVS**: Minimal Version Selection, Go's version-resolution algorithm.
- **ADR**: Architecture Decision Record (`docs/adr/NNNN-title.md`).
