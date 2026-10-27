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
2. Run the flow-sensitive ownership checks: use after move, `defer`/`go` borrows.
3. Elaborate drops: destructor calls on normal *and* unwind paths, and zeroing on `move`.
   - Moving a Go++ value copies its bytes and zeroes the source.
   - Moving a C++ value, implicitly or with `move(x)`, runs its move constructor; the source keeps its moved-from state and is destroyed at the end of its scope (P01 D1.4 rule 9). Relocation (P05 D5.5) is used only inside containers.
   - Drop glue skips `drop` on all-zero values.
4. Lower `!`, bounds checks and nil checks.
5. Monomorphize generic calls.
6. Run light GIR passes: constant folding, dead-block removal, trivial inlining (dev/release).
7. Emit LLVM IR.

**D4.3 — Monomorphization and where instances live.**
- **Debug and dev builds:** each generic instance gets one home per build: one object file per instance (or per batch of instances), keyed by a full fingerprint and shared by all packages. Packages reference it instead of compiling their own copy.
- **Release builds:** instances are emitted `linkonce_odr` in a COMDAT, so LTO can inline them. Duplicates fold at link time.
- **Fingerprint** = the instance's GIR after substitution + the fingerprints of every type and function it references + build mode + target.
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
| `view string` | `{ptr, len}` |
| `[]T` | `{ptr, len, cap}` |
| `view []T` | `{ptr, len}` |
| interface | `{itab*, data}` |
| func value | `{code*, env*}` |
| `*T`, `Unique[T]` | pointer |
| `Shared[T]`, `Weak[T]` | pointer to the control block |
| `map[K]V`, `chan T` | pointer to the runtime object |

**D4.6 — Panics are native C++ exceptions (Itanium ABI).**
- The runtime defines a `type_info` for the panic payload and throws with `__cxa_allocate_exception`/`__cxa_throw`. So a C++ `catch (...)` that swallows a panic still frees it correctly.
- Go++ frames use `__gxx_personality_v0` with cleanup landing pads that run destructors.
- Functions containing `defer` also get a catch-all. **Deferred calls run inside the catch handler,** so nested panics follow P01 D1.9. The handler implements `recover`, and rethrows if the panic was not recovered. A deferred call may block and resume on another thread, so goroutine switches save the C++ exception state (P12 D12.4). In `main.main`, a panic still active after the deferred calls first stops the other goroutines (P12 D12.12), so `main`'s locals are dropped last, as at a normal exit.
- **Catch-all trampolines** at `main`, at goroutine entry and at C-callback entry print an unrecovered panic, stop the other goroutines (P12 D12.12) and exit with code 2.
- **One unwinder per binary:** libunwind with libc++; libgcc_s with libstdc++.
- Calls to functions known to be `nounwind` get no landing pad.
- **Under `-panic=abort`:** Go++ code gets no cleanup landing pads. Calls to C++ functions that may throw get abort-on-unwind landing pads, and functions keep `uwtable`, so stack traces still work.
- Windows support (MSVC EH with funclets) is tier-3 work (P16).

*Why:*
- Go++ and C++ share one unwinder and one personality routine. C++ exceptions and Go++ panics then cross each other's frames with destructors running and no glue code. libc++abi and libunwind are bundled for C++ interop anyway (P08).
- Foreign exceptions can't nest in libc++abi (it calls `std::terminate`), so a panic must be a native C++ exception.
- Without a handler at the bottom of the stack, the program terminates with exit code 134 and no message. The trampolines are that handler.

*Rejected:* a custom personality routine (duplicates libc++abi, needs interop glue); setjmp/longjmp (skips C++ destructors and slows the happy path).

**D4.7 — DWARF 5 from day one.** M0 emits line tables; M1 adds variables and types. Type names use Go++ spelling (`main.Doc`, `[]string`). Pretty printers come in P14.

**D4.8 — Three build modes, one semantics.**
- `debug`: no GIR inlining, LLVM `-O0`, FastISel.
- `dev`: the default for `gopp build`. GIR trivial inlining plus a small LLVM pass set, tuned in P06.
- `release`: LLVM `-O2` (`-O3` opt-in), ThinLTO optional.

Bounds, nil and division checks stay on in every mode unless explicitly disabled with `@NoBounds` or a per-package flag.

*Risk:* `invoke` and x86-64 FastISel. FastISel can't select `invoke`, so blocks that end in `invoke` fall back to the slower SelectionDAG. See the P04.15 spike.

**D4.9 — Linking with the bundled LLD.**
- `ld.lld` runs as a subprocess; `ld64.lld` and `lld-link` follow later. Library-mode LLD only if measurements justify it.
- crt objects and the dynamic linker come from host sysroot discovery (P00 D0.12).
- The runtime is a per-target static archive, built from source and cached (P05/P06).

**D4.10 — Differential testing against Go.** This is the M1 gate.
- A corpus of programs written in the Go-compatible subset (no ownership-specific syntax) runs under both `go run` and `gopp run`.
- Go 1.25 is pinned (P01 D1.1). It is already installed on the dev machine.
- The harness compares stdout, normalized stderr and the exit code.
- A tagged "expected differences" list covers the places where Go++ differs on purpose (e.g. map insert during `range`, cooperative scheduling, a slice kept from an interface parameter (P01 D1.12)).
- Programs that Go++ rejects are counted separately.
- The corpus reuses the "run" tests in `$GOROOT/test`.

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

- **Q1** GIR serialization format details (coordinated with P06.02).
- **Q2** (deferred to P06) One LLVM module per package, or per function group in debug mode? Decided by measurement in P06.07 (see P06 D6.5).
