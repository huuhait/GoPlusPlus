# P05 — Runtime and memory

**Goal:** implement the no-GC memory model and the runtime core: startup, allocation, owned built-in types, drop glue, panics with readable stack traces, interface dispatch, safety checks and sanitizers.
**Depends on:** P04 · **Unblocks:** P06, P08, P12, P13 · **Size:** L · **Milestone:** M2

## Decisions

**D5.1 — The runtime is Go++ (with `unsafe`) plus small C/assembly pieces, and it links against libc.**
- Writing the runtime in Go++ dogfoods the language and lets the runtime inline through GIR.
- It calls libc (glibc, musl, libSystem, UCRT) rather than making raw syscalls the way Go does. C/C++ interop requires sharing `malloc`, threads, TLS and stdio with the imported libraries.

**D5.2 — Allocation API.**
- `mem.Allocator` provides alloc, resize and free, each taking a size and an alignment.
- In v0 the default global allocator is libc `malloc`/`realloc`/`free`. Bundling mimalloc as the default is evaluated in P15.10.
- `new_unique`, `new_shared`, `make`, and string and slice growth all use the default allocator. Per-container allocators depend on P01 Q4.

**D5.3 — Arenas.**
- `mem.Arena` is a chunked bump allocator with `Reset` and `Free`.
- `mem.ArenaNew[T Copy](a *mem.Arena) *T` returns raw pointers owned by the arena.
- In v0, `ArenaNew` accepts only `Copy` types, so no destructors need tracking. Arenas with registered destructors may come later.
- **Allocator objects.** Owned slices and maps store one pointer to an allocator object (P01 D1.22). The object starts with its function table (alloc, resize, free); nil means the global allocator. `mem.Arena` is one such object.
- `mem` functions create a slice or map in a given allocator (names set in P05.03). Resetting or freeing an arena while its slices or maps are alive leaves them dangling (P01.14).

*Why:* this is the fastest pattern for compilers, parsers and request-scoped work, with zero bookkeeping.

**D5.4 — Built-in owned types.**
- **`string`:** `{ptr, len, cap}`.
  - Literals are static: cap 0, never freed, and cloning one is free.
  - `+` and `+=` allocate or grow.
  - No small-string optimization in v0 (P15 Q1).
- **`[]T`:** `{ptr, len, cap, alloc}` (P01 D1.22). Go's growth policy (doubling for small slices, easing toward 1.25×). Elements are dropped when removed or when the slice is destroyed.
  - Re-slicing a slice into itself (`s = s[i:j]`) shrinks it in place: the cut elements are dropped and the capacity is kept (P01 D1.20).
- **`map[K]V`:** an open-addressing Swiss table, as in Abseil and Go 1.24. The map header also stores its allocator (P01 D1.22).
  - The hash seed is random per process, and iteration starts at a random position, so iteration order is unspecified as in Go.
  - Keys and values are dropped on delete, clear and destroy. Inside a `range`, the entry the loop is on is dropped at the end of that iteration, so `k` and `v` stay valid (P01 D1.21).
  - The compiler generates hash and equality functions per key type.
  - Lookups accept a `ref` key, so `counts[w]` with `w ref string` doesn't allocate (P01 D1.6).
  - Maps count their active iterators. Inserting a new key while an iterator is active **always** panics with "map insert during iteration", whether or not the table has room (P01 D1.21, ADR-0006). Writing an existing key is allowed.
- **`Unique[T]`** (`new_unique`): allocated on creation; the target is dropped and freed on destruction.
- **`Shared[T]`** (`new_shared`): one allocation holding both the control block (atomic strong and weak counts) and the value.
- **`Weak[T]`** (`new_weak`, from a `Shared[T]`): holds a weak count; `Lock()` upgrades it to a `Shared[T]`.

**D5.5 — Drop glue and relocation.**
- The compiler generates drop glue for every owning type: call `drop` (if defined), then destroy the owning fields in reverse order.
- Glue is emitted `linkonce_odr` and is inlinable.
- **All-zero skip:** drop glue skips the user `drop` when the value is all zero bytes. So dropping a zero value is a no-op in every profile, and zero-on-move depends on it.
- **No unwinding:** drop glue is `nounwind`. A panic that escapes `drop` aborts the program with a clear message (P01 D1.9).
- **Iterative drop:** `Unique[Self]` chains (linked lists) are dropped iteratively, and trees use a heap work list, so long lists don't overflow goroutine stacks.
- **errno:** drop glue preserves errno, so a drop that runs right after a C call doesn't change the errno the caller reads (P07 D7.7).
- **Relocate:** each type has a relocate operation that moves a value to a new address. Containers use it, for example when a slice grows or a map rehashes.
  - `memcpy` for Go++ types and trivially relocatable C++ types;
  - move constructor plus destructor for everything else. It never uses `realloc` or byte swaps for those.

**D5.6 — Interfaces and type descriptors.**
- **Static itabs:** each (concrete type, interface) itab is generated at conversion sites as a `linkonce_odr` global, so the common path never builds an itab at runtime.
- **Dynamic conversions:** interface-to-interface conversions and assertions use a runtime cache keyed by (type, interface).
- **Descriptors:** a type descriptor records size, alignment, kind, the drop/clone/relocate/hash/eq functions, a "has a zero value" flag, the name and the method table. For types converted to an interface, it also carries the field table (names, offsets, types, struct tags) that `fmt` and `encoding/json` read (P01 D1.12).
- **Identity:** comes from `linkonce_odr` symbols, with a fallback name comparison across shared objects.

**D5.7 — Panics and stack traces.**
- The exception object is a native C++ exception (P04 D4.6). It carries the panic value (`any`, moved in), a backtrace captured at panic time (`_Unwind_Backtrace`) and a recovered flag.
- **Go-exact nesting** (P01 D1.9): a panic in a deferred call replaces the current one, and a later `recover()` can recover it. An unrecovered chain prints all panics and exits with code 2.
- **Trace data:** a compact runtime line table (like Go's), which survives `strip`. It counts in the P15 size target.
- Symbolization happens lazily, only when a trace is printed.
- Traces look like Go's: `main.load(...)` followed by `\t/src/main.gopp:12 +0x1c`.
- **Trampolines:** the catch-all trampolines from P04 D4.6 (at `main`, goroutine entry and C-callback entry) print an unrecovered panic, stop the other goroutines (P12 D12.12) and exit with code 2.

**D5.8 — Explicit safety checks.**
- Bounds, nil dereference, integer division by zero and negative shift counts are checked with a compare plus a branch to a cold panic call.
- No SIGSEGV tricks for nil checks. Signal handling stays free for the C code that needs it.
- `@NoBounds` (per function) and a per-package flag turn bounds checks off.

**D5.9 — Sanitizers are one flag away.**

| Flag | Notes |
|---|---|
| `-asan` | Includes the LSan leak check at exit. Goroutine stacks use the ASan fiber API and are registered as LSan root regions |
| `-ubsan` | |
| `-msan` | Linux only. Requires instrumented C/C++, which the cdep pipeline rebuilds with the same flags |
| `-race` | TSan (P12). Goroutine switches use `__tsan_switch_to_fiber` with the no-sync flag, plus explicit acquire/release at real hand-offs between goroutines |

- Each flag adds the matching LLVM sanitizer passes and links the compiler-rt runtime, bundled per target.
- The runtime, std and libc++ are built per (target × mode × sanitizer). cdeps are rebuilt the same way (P11 D11.7).
- P05.12 delivers the flags for single-goroutine programs. The goroutine parts (ASan fibers, LSan root regions, TSan switches) come with the scheduler (P12.09), and libc++ per sanitizer with P08.10.
- The docs list which targets each sanitizer supports. None of them work on static musl.
- There is no custom debug allocator; ASan covers that need.

**D5.10 — Startup and exit.**
- Startup runs: crt → a generated C `main` → runtime init (args, env, hash seed) → package `init`s in dependency order → `main.main` → exit.
- Exit follows P01 D1.18. After `main.main` returns and its deferred calls have run, the runtime:
  1. stops all other goroutines (P12 D12.12; a no-op until the scheduler exists);
  2. drops `main`'s locals;
  3. calls C `exit()`, which runs atexit handlers, flushes stdio and runs C++ static destructors.
- `os.Exit(code)` skips defers and drops, stops the other goroutines, and calls `exit(code)`.
- Package-level variables are never destroyed.
- `os.Stdout` is unbuffered, as in Go.
- Until P08.10, the EH runtime comes from the host (P04 D4.11).

**D5.11 — Errors and error traces** (P01 D1.19).
- **Error values:** package-level errors from `errors.New` are static: never freed, never counted. Other errors live in reference-counted boxes, like other interface values.
- **Trace:** an error returned through `!` records each `!` site it passes (P01 D1.8).
  - The trace lives in a per-goroutine ring keyed by the error's identity, with no allocation per hop. Entries are written only on the error path.
  - It never wraps the error, so `==`, `errors.Is`, `errors.As`, `%w` and type switches see the original error.
  - A `!` panic and `%+v` print the whole chain. After an error crosses goroutines, its trace is best effort.
- Task P05.14 builds both.

**D5.12 — Bootstrap std.** Early in P05 (task P05.15), a minimal set of std functions, so samples and differential tests can print to stdout:
- `fmt`: `Println`, `Printf` with the common verbs;
- `os`: `Args`, `Open`, `Exit`;
- `io.ReadAll`;
- `errors.New`, and `errors.MustError`, the `!` panic value (P04.10).

P13's full packages replace them later.

## Deliverables

- `runtime/` (Go++ + C) and `std/mem` (`Allocator`, `Arena`).
- Compiler support for drop glue, type descriptors, itabs and safety checks.
- A panic runtime with symbolized traces.
- Sanitizer flags with bundled compiler-rt.
- The M2 sample suite running under ASan/LSan in CI.

## Exit criteria

- M2: every P01 sample that uses no concurrency, no C/C++ imports and no macros runs correctly and leak-free under ASan + LSan.
- panic/defer/recover conformance tests pass, including destructor order during unwinding.
- Microbenchmarks recorded:
  - map insert/lookup vs `absl::flat_hash_map`: target ≤ 1.2×;
  - slice append vs `std::vector::push_back`: target ≤ 1.1×.

## Risks

- **Code size from drop glue and descriptors** → `linkonce_odr`, outlined drops for large types, descriptors emitted only when used.
- **Unwinding cost** → `nounwind` inference, `-panic=abort`.
- **Deep recursive drops** (long linked lists) overflowing the stack → handled by iterative drop glue: `Unique[Self]` chains are dropped in a loop, and trees use a heap work list (D5.5).
- **Static binaries with glibc are impractical** → fully static musl builds (P07.14).

## Open questions

- **Q1** Arenas with destructors (D5.3)?
- **Q2** (deferred to P15 Q2) mimalloc as the default allocator (P15.10)?
- **Q3** (decided 2026-10-27) Package-level variables are never destroyed at exit, not even as an opt-in (P01 D1.18).
