# P04 — GIR and LLVM code generation

**Goal:** lower the typed AST through GIR to LLVM IR, object files and a linked executable for Linux x86-64 and arm64. First a hello-world slice (M0), then the whole Go-core subset, checked by differential tests against Go (M1).
**Depends on:** P03; M1 also needs the bootstrap std (P05.15) · **Unblocks:** P05, P06, P07, P09, P15 · **Size:** XL · **Milestone:** M0, M1

## Decisions

**D4.1 — A mid-level IR (GIR) between the AST and LLVM IR.** GIR is typed, CFG-based and in place/rvalue form, like Rust MIR or Swift SIL. It is not SSA; LLVM's mem2reg/SROA build SSA for us.
*Why:*
- Drop elaboration and move checking need explicit places and cleanup edges.
- Monomorphization and cross-package inlining need a compact, serializable IR for export data (P06), more stable than LLVM bitcode.
- The macro interpreter executes GIR (P09).
- Simplifying at the GIR level cuts the work LLVM has to do. This is the main compile-speed lever.

*Rejected:* going from the AST straight to LLVM IR, as Clang does. Ownership, drops and generic instantiation would end up scattered through codegen.

**D4.2 — Per-function pipeline.**
1. Build GIR.
2. Run the flow-sensitive ownership checks: use after move, `defer`/`go` borrows, then the borrow checker (D4.16, P04.20), then the concurrency checks (D4.17, P04.21). A borrowed map range variable counts as moved after `move(m[k])` of its entry (P01 D1.4 rule 11, ADR-0016). The comma-ok reads (`u, ok := x.(T)`, `clone(m[k])`, `move(m[k])`, `<-ch`, `o.Get()` for a type with no zero value, used only where `ok` is true), the `v, err := f()` reads (only where `err == nil` when `v`'s type has no zero value) and the known-non-nil stores are checked by the type checker's flow typing (P03.18, ADR-0018 amendment point 4); here they are only GIR regression cases. A swap of places evaluates every index, key and pointer operand, with its bounds and nil checks, before it moves any value (OWN-R4). A value stored in an `error`, `any`, map or channel `@Required` field is known non-nil (spec TYP-15's list), so reads of such a field skip the nil check.
3. Elaborate drops: destructor calls on normal *and* unwind paths, and zeroing on `move`.
   - Moving a Go++ value copies its bytes and zeroes the source.
   - Moving a C++ value, implicitly or with `move(x)`, runs its move constructor; the source keeps its moved-from state and is destroyed at the end of its scope (P01 D1.4 rule 9). Relocation (P05 D5.5) is used only inside containers.
   - Drop glue skips `drop` on all-zero values.
4. Lower `!`, bounds checks and nil checks. Nil checks are only for channels and `error` / `any` values: the pointer kinds, funcs and interfaces with methods are never nil, so their uses have none (ADR-0018 and its amendment point 3). Map writes need no nil check, because P04 passes the address of the owning map header and a write to a nil map creates it there (amendment point 5). There is no exception: a write into a temporary map or into a map inside an interface is a compile error (P03.19), so P04 never lowers one.
5. Monomorphize generic calls.
6. Run light GIR passes: constant folding, dead-block removal, trivial inlining (at every `opt`; the `opt 0` pipeline keeps it small).
7. Emit LLVM IR.

**D4.3 — Monomorphization and where instances live.**
- **Per-instance `Option` check (ADR-0018 point 11):** the type checker checks a generic body once, so an `Option[T]` written or inferred inside it (`First[U](xs)` giving `Option[U]`) is not validated for a nilable or `Option` type argument. Substitution must re-check every `Option[X]` it creates and report the instantiation as an error. `gopp check` (P04.09) must run the same per-instance validation for the instances it sees, so a check-only run does not miss the error. The same per-instance check covers a checked result whose last result is a type parameter instantiated with `error` (`call[error](loadN)`): P03 treats only a last result written as `error` as a guard (OWN-P10).
- **Debug builds (`opt 0` or `opt 1`):** each generic instance gets one home per build: one object file per instance (or per batch of instances), keyed by a full fingerprint and shared by all packages. Packages reference it instead of compiling their own copy.
- **Release builds (`opt 2` and up):** instances are emitted `linkonce_odr` in a COMDAT, so LTO can inline them. Duplicates fold at link time.
- **Fingerprint** = the instance's GIR after substitution + the fingerprints of every type and function it references + the effective profile settings that affect codegen + target.
- The instance homes live in the build cache (P06.06). Instance symbol names follow D4.13.

*Why:* without one home, every package compiles its own copy of each instance, and debug builds pay for it many times.

**D4.4 — Calling conventions: internal for Go++, the exact C ABI for C.**
- **Go++ → Go++ calls** use an internal convention that returns `(T, error)` and other aggregates in registers, like Go's ABIInternal (P04.18).
- **C-ABI entry points:** functions used as C callbacks or marked `@Export` get C-ABI thunks, like Go's ABI0 wrappers.
- **C calls** use the exact C ABI. Clang's `CodeGenABITypes` only *classifies* a signature, so Go++ writes the lowering for each `ABIArgInfo` kind itself. P04.11 is sized L.
- **Parameters** (P01 D1.5):
  - trivial parameters are copies;
  - borrowed owning parameters pass by hidden pointer;
  - borrowed `string`/`[]T` parameters pass as `(ptr, len)`;
  - shared-handle parameters pass without a retain, and the callee retains a copy it keeps;
  - interface parameters pass as `(itab, data)`, with a tag for a borrowed value, which is cloned into a box only if the callee keeps it (P01 D1.12).

*Why:* with the C convention everywhere, a `(T, error)` result larger than 16 bytes comes back through memory.

**D4.5 — Data layout.**

| Type | Layout |
|---|---|
| struct | declaration order, C alignment |
| `string` | `{ptr, len, cap}`; a literal points into rodata with cap 0 and is never freed |
| `View[T]` where `T`'s underlying type is `string` or a slice (`View[string]`, `View[Bytes]`) | `{ptr, len}` |
| `[]T` | `{ptr, len, cap}` |
| `View[T]` (every other `T`, including map, chan, func, interface, `Option`, structs) | `{ptr}` |
| interface | `{itab*, data}` |
| func value | `{code*, env*}` |
| `*T`, `Unique[*T]` | pointer |
| `Shared[*T]`, `Weak[*T]` | pointer to the control block |
| `Option[P]` for a pointer kind `P` | the same as `P`; the null bit pattern is `nil` |
| `Option[I]` for an interface `I` with methods (not `error`; `any` keeps `nil` and has no `Option`), `Option[F]` for a func type `F` | the same as `I` / `F`; the nil bit pattern (null `itab*` / null `code*`) is `nil` (ADR-0018 amendment point 3) |
| `Option[T]` for other `T` | `T` followed by a one-byte "is set" flag (padded to `T`'s alignment) |
| `map[K]V`, `chan T` | pointer to the runtime object |

`Option[T]` is comparable when `T` is (ADR-0018 amendment point 1): GIR `==` on it is "both nil, or both set and `T`'s equality"; for a pointer kind it is the plain pointer compare, and for an `Option[I]` the nil bit pattern (null `itab*`) is nil. The flag byte and its padding must not take part in the comparison or the hash (P05 D5.4, map keys).

**D4.6 — Panics are native C++ exceptions (Itanium ABI).**
- The runtime defines a `type_info` for the panic payload and throws with `__cxa_allocate_exception`/`__cxa_throw`. So a C++ `catch (...)` that swallows a panic still frees it correctly.
- Go++ frames use `__gxx_personality_v0` with cleanup landing pads that run destructors.
- **Defer-extended locals (P01 D1.9, ADR-0006 D17):** a block-scoped local used by a `defer` gets per-execution storage. The defer record owns the local. The block-end drop is skipped when the record exists, and the local is dropped after all deferred calls have run, with the function-scope locals.
- Functions containing `defer` also get a catch-all. **Deferred calls run inside the catch handler,** so nested panics follow P01 D1.9. The handler implements `recover`, and rethrows if the panic was not recovered. A deferred call may block and resume on another thread, so goroutine switches save the C++ exception state (P12 D12.4). In `main.main`, a panic still active after the deferred calls first stops the other goroutines (P12 D12.12), so `main`'s locals are dropped last, as at a normal exit.
- **Catch-all trampolines** at `main`, at goroutine entry and at C-callback entry print an unrecovered panic, stop the other goroutines (P12 D12.12) and exit with code 2.
- **One unwinder per binary:** libunwind with libc++; libgcc_s with libstdc++.
- Calls to functions known to be `nounwind` get no landing pad.
- **Under `-panic=abort`:** Go++ code gets no cleanup landing pads. Calls to C++ functions that may throw get abort-on-unwind landing pads, and functions keep `uwtable`, so stack traces still work.
- Under `-panic=abort` a panic prints the trace and exits with code 2, `recover()` returns nil and nothing unwinds. Whether deferred calls run on that path is decided in P01.06.
- Windows support (MSVC EH with funclets) is tier-3 work (P16).

*Why:*
- Go++ and C++ share one unwinder and one personality routine. C++ exceptions and Go++ panics then cross each other's frames with destructors running and no glue code. libc++abi and libunwind are bundled for C++ interop anyway (P08).
- Foreign exceptions can't nest in libc++abi (it calls `std::terminate`), so a panic must be a native C++ exception.
- Without a handler at the bottom of the stack, the program terminates with exit code 134 and no message. The trampolines are that handler.

*Rejected:* a custom personality routine (duplicates libc++abi, needs interop glue); setjmp/longjmp (skips C++ destructors and slows the happy path).

**D4.7 — DWARF 5 from day one.** M0 emits line tables; M1 adds variables and types. Type names use Go++ spelling (`main.Doc`, `[]string`). Pretty printers come in P14.

**D4.8 — Build profiles, one semantics.** (Changed by ADR-0007; P06 D6.12 and D6.6 define the profiles and pipelines.)
- `opt 0` (the default `debug` profile): GIR trivial inlining plus a small LLVM pass set, tuned in P06.
- `opt 3` with `lto thin` (the default `release` profile): LLVM `-O3`; `opt 2` and `lto full` are settings you can choose.
- The plain `-O0` + FastISel pipeline (no GIR inlining) stays as a measuring baseline (`-pipeline=fastisel`).
- Where this document says "debug/dev build" it means `opt 0` or `opt 1`; "release build" means `opt 2` or higher.

Bounds, nil and division checks stay on in every profile unless explicitly disabled with `@NoBounds` or a per-package flag.

*Risk:* `invoke` and x86-64 FastISel. FastISel can't select `invoke`, so blocks that end in `invoke` fall back to the slower SelectionDAG. See the P04.15 spike.

**D4.9 — Linking with the bundled LLD.**
- `ld.lld` runs as a subprocess; `ld64.lld` and `lld-link` follow later. The LLD bundled inside `gopp` is started as `gopp lld …` (one file to ship; owner decision, P06 Q3). Library-mode LLD only if measurements justify it.
- crt objects and the dynamic linker come from host sysroot discovery (P00 D0.12).
- The runtime is a per-target static archive, built from source and cached (P05/P06).

**D4.10 — Differential testing against Go.** This is the M1 gate.
- A corpus of programs written in the Go-compatible subset (no ownership-specific syntax) runs under both `go run` and `gopp run`.
- Go 1.25 is pinned (P01 D1.1). It is already installed on the dev machine.
- The harness compares stdout, normalized stderr and the exit code.
- A tagged "expected differences" list covers the places where Go++ differs on purpose (e.g. map insert during `range`, cooperative scheduling, a slice kept from an interface parameter (P01 D1.12)).
- Programs that Go++ rejects are counted separately.
- The corpus reuses the "run" tests in `$GOROOT/test`.
- Go's outputs are made once and saved as golden files. They are regenerated only when the Go version or the test changes, so test runs never call `go run`.

**D4.11 — Bootstrap mode.** It is used until P06, P07 and P08 exist. Task P04.16 builds it.
- (a) **Source mode:** loads all packages from source and emits one object per package. The early LSP uses it too.
- (b) **ABI classification:** a minimal embedded Clang classifies C signatures (D4.4). This part of P07.01 moves here.
- (c) **EH runtime:** comes from the host (libgcc_s/libstdc++, or the nixpkgs libc++abi/libunwind) until P08.10.
- (d) **Runtime C/asm parts:** CMake builds them as part of building `gopp`, until `gopp cc` exists.
- (e) **Tests:** use the bootstrap std (P05 D5.12) and print to stdout. (Go's `println` writes to stderr.)

**D4.12 — Go semantics on LLVM.** Go defines a result for some operations that are undefined behavior in LLVM (the P01 D1.17 cases). P04.04 lowers them like this:
- **Division:** signed `/` and `%` check the divisor for -1, so `MinInt / -1` gives `MinInt` and `MinInt % -1` gives 0, with no trap.
- **Shifts:** `select`, or masking when the range is known, so a shift by ≥ the bit width gives Go's result.
- **Float → int:** `llvm.fptosi.sat`/`llvm.fptoui.sat`.
- **FMA:** contraction matches Go's compiler on each target.
- **Bounds and nil:** explicit checks, as in P05 D5.8.

Each case is also in the UB list (P01.14) and in the differential corpus.

**D4.13 — Symbol names.** Task P04.17 builds them.
- Derived from content, never from creation order.
- Stable across packages.
- Readable in traces, gdb and perf.
- A fixed prefix, so they never collide with C names.
- Specified in a spec appendix, with a demangler.

**D4.14 — A fast test runner** (P00 D0.13; task P04.19).
- Run tests compile many programs in one `gopp` process, on every core, with the runtime and std from the cache.
- Most run tests execute through LLVM's ORC JIT, with no linking. The code runs in a pool of executor processes (ORC supports running JIT code in another process), so a crash or `os.Exit` ends only that executor, which is restarted.
- A smaller set builds real binaries: linking, C interop, exit codes and signals.
- Stage tests stop early (`--dump-ast`, `--dump-gir`) and check the dump with FileCheck.

**D4.15 — GIR serialization format** (owner decision; decides Q1). How GIR bodies are saved in export data (`.gopx`, P06 D6.2), so importers can instantiate generics and inline without re-reading the source.
- **Encoding: LLVM's bitstream container** (`llvm/Bitstream`), the format used by LLVM bitcode, Clang PCH/modules and Swift's `.swiftmodule`, which also stores generic and inlinable bodies. It is already in LLVM (no new dependency), compact, and blocks carry their length, so a reader jumps straight to one function through the index. It is only the container: LLVM tools cannot read GIR inside it (`llvm-bcanalyzer` still shows the block layout).
- **Version: only the exact same compiler reads it.** The file header holds the compiler version; any mismatch means "rebuild", as in Go. No compatibility across versions.
- **Deterministic:** the same input gives byte-identical output (stable ordering, no thread-order or pointer-order effects), because the cache hashes these bytes (P06 D6.2, D6.3).
- **Text dump:** `gopp gir dump <file.gopx>` prints the GIR with the P04.01 printer; tests round-trip binary → text → binary and require identical bytes.
- P04.01 provides the serialization hooks; P06.02 writes and reads the `.gopx` sections.

## From M0 to M1, in order

1. `package main; func main() { println("hello") }`, with `println` written in Go++ on top of `@Extern("C") func write(fd int32, p *byte, n uint) int`.
2. Integers, floats, bools; functions; `if`/`for`/`switch`.
3. Structs, arrays, raw pointers, string literals.
4. The rest of the Go core: slices, maps (a temporary simple runtime), methods, interfaces, closures, generics, `defer`/`panic`/`recover`, `!`.

## Deliverables

- `compiler/lib/GIR` (builder, verifier, printer, passes, serialization hooks) and `compiler/lib/CodeGen`.
- `gopp build -emit=gir|llvm|asm|obj` and `-dump-gir`.
- Tests:
  - GIR FileCheck tests;
  - LLVM IR FileCheck tests;
  - run tests;
  - differential tests (≥ 100 programs);
  - ABI tests against Clang-compiled C.

## Exit criteria

- M0: hello runs on linux/amd64 and on linux/arm64, each built natively (cross builds come with musl in P07.14).
- M0 builds on NixOS and on an FHS distro (a Debian/Ubuntu container).
- M1: at least 100 differential programs match Go's output.
- The GIR verifier runs in every test, and the compiler is ASan-clean on the full suite.
- gdb and lldb can step through the samples and print locals.

## Risks

- **ABI corner cases** → Clang classifies every C signature (D4.4); generated ABI tests in P07.12.
- **`invoke` makes debug codegen slow on x86-64** → the P04.15 spike runs before P04.07.
- **LLVM compile time** → measure from the first slice (`-trace`, P06.09) and keep GIR output lean.
- **EH overhead in hot code** → `nounwind` inference and `-panic=abort`; measured in P15.

## Open questions

- **Q1** (decided 2026-10-27) GIR serialization format: LLVM bitstream container, same-compiler-version only, deterministic bytes, `gopp gir dump` (D4.15).
- **Q2** (deferred to P06) One LLVM module per package, or per function group at `opt 0` (the debug profile)? Decided by measurement in P06.07 (see P06 D6.5).
- **Q3** (open, decide before P04.20 and P04.21 are built) Do `.go` files get the borrow checker and the concurrency checks (ADR-0022)? They have no GC in Go++ either, so the assumption is yes, with the same rules (ADR-0021 open point 3, ADR-0022 open point 4).

**D4.16 — Borrow checker** (ADR-0021; task P04.20).
- It runs on the same flow graph as the use-after-move check (D4.2 step 2), after it, in the same per-function pass. No lifetime annotations: borrows are found from the code, and liveness is computed backwards from the uses (non-lexical lifetimes).
- Each borrow records the place it depends on (a variable, a prefix of a path, an arena). A "change" (assign, `append`, `move`, `clear`, arena `Reset`, a write of an owning element or field, a map insert or delete) of that place or a prefix of it, at a point where the borrow is live, is an error. A pointer-free Copy write is not a change.
- Calls (amendment A, 2026-10-28): a borrow result depends only on the arguments in the callee's **borrow summary**, computed from the body: one bit per parameter and receiver, the union over all return paths. Mutually recursive functions iterate to a fixpoint (start empty, stop when nothing changes; linear in practice). Summaries are stored with the function and in export data (P03.11), together with P04.09's "writes this parameter" bit; a changed summary is an API change for `gopp vet` / the API diff (P14.01). Where there is no body (interface methods, func values of unknown origin, C/C++, extern or assembly functions) the result depends on every borrow-capable argument and the receiver. There is no lifetime syntax. *(Was: elision, every call result depends on every borrow-capable argument and the receiver.)*
- Exclusive writable access (amendment B, spec OWN-B9): a borrow record has a mode, read or write. A write borrow of a place conflicts with any live borrow of an overlapping place; overlap is a path-prefix test, with constant indexes, different fields, different variables and `Split` results known disjoint. A copy of a writable pointer is a reborrow: the source is blocked until the copy's last use. Every raw `*T` is a recorded borrow, so no run-time check is needed. `Shared[*T]` writes are rejected except cell-typed parts (`mem.Cell`, `Mutex`, `RWMutex`, `Atomic`), used through their own methods. The pass stays linear: a conflict is looked up per place, not per pair of borrows.
- A writable argument and an overlapping borrow argument at one call is an error (the call case of the rule above).
- Escapes (a global, a heap container, a channel, an escaping closure, `go`, an escaping interface box) are checked where the borrow is stored or captured.
- Cost: one backward liveness pass and one forward pass per function, linear in function size. A test measures it on a large generated function.
- `gopp check` runs it. `.go` files use the same pass (open point in ADR-0021).

**D4.17 — Concurrency checks** (ADR-0022; task P04.21).
- They run on the same flow graph, after the borrow checker (D4.16), in the same per-function pass, because they use its borrows (a guard is a borrow of its mutex, a `sync.Scope` goroutine borrows locals).
- **Labels.** `Send` and `Sync` are computed per type, once, from the parts of the type (a struct has a label only if all fields have it), and cached in the type table. They go into export data with the type (P03.11, P06), so a package that imports a type gets the same label. They are not written by hand and cannot be implemented by the user. They also act as the predeclared constraints `[T Send]` and `[T Sync]` (checked per instance, like D4.3).
- **`go` and `s.Go`.** For each argument and each captured variable the pass decides: moved in (the variable is dead in the spawner after the `go`), copied in (`Send`), or shared (`Sync`). A writable captured variable that is live in the spawner or captured by another goroutine is an error. In a `sync.Scope`, closures given to `s.Go` may also borrow; two writable borrows of one place across goroutines of one scope are an error (the same overlap test as OWN-B4); the two results of `Split` are disjoint places.
- **Globals.** A write to a package variable outside `init` and its initializer needs a `Sync` type.
- **Deadlock check.** Locks are identified by lock class (like Linux lockdep): the same field of the same type, or the same package variable. The pass records, per function, the ordered pairs "a lock of class A was taken while the guard of class B was live". The pairs go into export data. Two functions with the pairs (A,B) and (B,A) are an error; so is locking the same mutex while its own guard is live, and so is taking two locks of the same class with both guards live (`sync.LockAll` is the fix; it is known to the pass). Locks behind interfaces or function values are not recorded.
- Cost: linear in function size, like D4.16; plus one pass over the recorded pairs per package. A test measures it on a large generated function.
- `gopp check` runs it.
