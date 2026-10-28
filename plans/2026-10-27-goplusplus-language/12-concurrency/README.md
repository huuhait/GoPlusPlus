# P12 — Concurrency

**Goal:** Go's concurrency model (goroutines, channels, `select`, `sync`) without a GC. C/C++ code must be able to run on goroutine stacks, and races must be detectable with TSan.
**Depends on:** P05, P07 (`ffi.Func` for callbacks from C threads) · **Unblocks:** P13 (net, os/exec, context) · **Size:** L · **Milestone:** M8

## Decisions

**D12.1 — Goroutines are stackful coroutines on fixed-size, non-moving stacks.**
- Go grows stacks by copying them. We can't: raw pointers into stacks are legal, and C/C++ frames live on them.
- Default reservation: 1 MiB on 64-bit targets (Q1, ADR-0013), with pages committed lazily by the OS, so a small goroutine uses only the pages it touches. Stacks use `MADV_NOHUGEPAGE`; a stack returned to the pool releases its touched pages beyond a small cached amount. 32-bit targets get a smaller default when they are added.
- `runtime.SetDefaultStack(n)` changes the default. `runtime.GoStack(n, f)` spawns a goroutine with an explicit size.
- The main goroutine runs on the main thread's stack.

**D12.2 — Stack-overflow detection: prologue checks plus guard pages (hybrid; Q1, ADR-0013).**
- **Prologue checks.** Every Go++ function compares `SP − frame size` with the goroutine's stack limit. It is the same compare as the D12.3 preemption check (sysmon sets the limit to a value that always fails, and the slow path tells preemption from overflow, as in Go), so it costs no extra instruction. Small leaf functions that fit in a fixed red zone skip it. An overflow panics with "goroutine stack overflow" and a trace.
- **Guard pages while the mapping budget allows.** Linux limits the number of memory mappings (`vm.max_map_count`, typically ≈ 65 k), and each guard page splits one, which caps guarded stacks at about 32 k. The runtime adds a guard page below each stack while its mappings stay under half the limit; past that, new stacks have none (Go++ code is still checked; only C overflows on those stacks are missed). Raising the sysctl brings them back. A SIGSEGV handler on an alternate signal stack reports a guard-page hit as "goroutine stack overflow".
- **C/C++ code.** A call into C or C++ needs at least 64 KiB (provisional) of stack left, or it panics before C runs. C/C++ built by gopp uses `-fstack-clash-protection`, so a large frame can't jump over a guard page. Go++ functions need no stack probes: the prologue check includes the frame size.

**D12.3 — Scheduler: M:N with work stealing (Go's G/M/P design).**
- Per-P run queues plus a global queue, with work stealing.
- A sysmon thread hands the P off from threads blocked in syscalls or long C calls.
- The number of spinning threads is limited.
- **Preemption:**
  - Yield points are channel operations, `sync` blocking, I/O and `runtime.Gosched()`.
  - Function prologues and loop back-edges also check a per-goroutine flag that sysmon sets (Go 1.13 style). So a tight loop can't starve other goroutines.
  - **Smart placement (Q3).** The compiler leaves the back-edge check out of loops with a small known trip count, and in other tight loops it checks only once every 1024 iterations (a counter in a register), so the cost in hot loops is close to zero. A goroutine may yield a few microseconds later; that's fine for fairness.
  - No async signal preemption (Q3): a goroutine only stops at these compiler-placed checks, never in the middle of C/C++ code.

*Why:* Go added async preemption in Go 1.14 for two reasons: tight loops that starve other goroutines, and its GC. The flag checks handle tight loops, and Go++ has no GC. Go's signal preemption also relies on telling Go code from C code at every instruction; in Go++, C++ code is inlined into Go++ functions (P08 D8.10), so a signal could stop a goroutine inside C++ code (for example holding `malloc`'s lock) and deadlock the thread. Erlang and GHC also use compiler/runtime checks rather than signals.

**D12.4 — Context switching is written in assembly, per ABI.**
- x86-64 SysV and AArch64 AAPCS64 come first; Win64 later.
- A switch saves the callee-saved registers and the FP control state.
- A switch also saves and restores the C++ exception state (`__cxa_get_globals()`: the caught-exception stack and the uncaught count). Deferred calls run inside a catch handler (P04 D4.6), and one that blocks may resume on another thread.
- Sanitizer fiber hooks (`__sanitizer_start_switch_fiber`, `__tsan_switch_to_fiber`) let ASan and TSan understand goroutines.

**D12.5 — Channels keep Go's semantics, made ownership-aware.**
- Go's semantics are kept as is: unbuffered rendezvous, buffered ring, close, blocking on nil channels, random choice among ready `select` cases, `for range ch`.
- `chan T` is a shared handle: atomically refcounted, and copying it retains it.
- Sending moves the value in; receiving moves it out.
- In `select`, a send case moves its value only if that case is chosen (P01 D1.13).
- A channel and the values still buffered in it are freed automatically when the last handle goes away (oldest first). There is no explicit `ch.Destroy()`: forgetting it would leak and calling it early would crash. Use `sync.Scope` when goroutines must finish first (P01 D1.13).

**D12.6 — The `go` statement.**
- Arguments are evaluated, then moved or copied into the new goroutine's frame.
- A closure used with `go` keeps its captured variables in shared cells (P01 D1.11). So the goroutine and the spawner see the same variables, as in Go.
- `go f(x)` with a raw pointer or a ref into a local is a compile error (P01 D1.13).

**D12.7 — `sync` and atomics.**
- `sync.Mutex`, `RWMutex`, `WaitGroup`, `Once` and `Cond` keep Go's API.
- The `sync` types are `@Pinned`: moving or copying one after first use is a compile error (P01 D1.4 rule 10).
- `sync.Scope` is an optional structured-concurrency helper. It waits for every function it spawned, so those closures may borrow without cells. **Designed here:** the marker that tells the compiler a closure given to `s.Go` cannot outlive the scope (for example `@Scoped` on the parameter of `s.Go`), see P01 CON-9.
- `sync/atomic` uses LLVM atomics, seq_cst like Go.
- Blocking primitives park goroutines, not threads, via futex on Linux. ulock (macOS) and WaitOnAddress (Windows) come after v0.1.

**D12.8 — I/O and timers.**
- A netpoller is integrated with the scheduler: epoll on Linux. kqueue (macOS/BSD) and IOCP (Windows) come after v0.1.
- Timers (`time.After`, `time.Ticker`, `select` timeouts) are integrated with the netpoller (P12.11).
- Blocking file I/O uses thread handoff. io_uring comes after v1.

**D12.9 — C, threads and TLS.**
- `runtime.LockOSThread()` pins a goroutine to its thread, for C libraries with thread affinity (OpenGL, GUI toolkits).
- **Every yield point is a thread-local barrier.** A goroutine may resume on another thread after a yield.
  - The runtime never keeps thread-local addresses across yields.
  - errno is read through a non-`const` accessor, because glibc declares `__errno_location` `const`.
- **Pinning.** A goroutine is pinned to its thread while C/C++ frames are on its stack, including during C → Go++ callbacks. So C thread-locals such as `errno` stay valid during a C call.
- **Foreign threads** that call Go++ are adopted (P07 D7.8).

**D12.10 — Race detector.** `gopp build -race` instruments Go++ code and the rebuilt C/C++ deps with TSan, and adds the fiber annotations. The TSan details are in P05 D5.9.

**D12.11 — Targets (provisional).**
- 100 k idle goroutines fit in ≤ 1 GiB RSS with default settings.
  - This holds with the default sysctl: past the mapping budget, new stacks skip the guard page (D12.2, ADR-0013).
- 1 M goroutines are possible after tuning the stack size and the mapping limit.
- Channel ping-pong and contended `Mutex` throughput are within 1.5× of Go on the same machine.

**D12.12 — Stopping goroutines at exit** (P01 D1.18).
- `main` returning, `os.Exit` and an unrecovered panic stop every other goroutine before C `exit()` runs.
- Each goroutine parks at its next yield point or preemption check and never resumes. Its stack is not unwound and its locals are not dropped.
- A thread inside a C call parks when the call returns.
- Before the scheduler exists there is only one goroutine, so this step is a no-op (P05.02).

## Deliverables

- **Runtime:** coroutines, scheduler with preemption flags, channels and `select`, parking, netpoller and timers, `LockOSThread`, pinning during C calls, adoption of foreign threads, stopping goroutines at exit.
- **Compiler:** `go` lowering with move semantics and shared cells, `select` lowering, preemption checks in prologues and loop back-edges, stack probes, TSan fiber integration.
- **Std:** `sync` (including `sync.Scope`), `sync/atomic`, `time` timers integrated with the netpoller.
- **Samples:** worker pool, pipeline, fan-in/fan-out, `select` with timeouts, TCP echo server.

## Exit criteria

- M8: the samples are correct, stress tests pass, and everything is TSan-clean.
- `-race` detects seeded data races.
- With one P, a spin loop on an atomic flag doesn't hang (D12.3). Timers fire while all Ps are busy (D12.8).
- The D12.11 numbers are recorded and met, or an ADR explains each gap.

## Risks

- **Fixed stacks use more memory than Go's growable stacks** → lazy commit, tunable sizes, the trade-off documented.
- **C code with large stack usage** → per-spawn stack sizes, `LockOSThread` plus dedicated threads, clear overflow diagnostics.
- **Subtle scheduler bugs** → port the ideas behind Go's runtime tests, and run long stress tests under TSan in CI.

## Open questions

- **Q1** (decided 2026-10-27, ADR-0013) Default stack size, and guard pages vs prologue checks (P12.02): both, prologue checks always and guard pages while the mapping budget allows (D12.2); a 1 MiB default reservation on 64-bit (D12.1).
- **Q2** (decided 2026-10-27) io_uring for file I/O: not in v1. File I/O uses thread handoff, as in Go (D12.8), on every OS. io_uring may come after v1 as an optional Linux-only speedup with the thread path kept as the fallback (it is Linux only, and often disabled, e.g. by Docker's default seccomp profile). User code doesn't change either way.
- **Q3** (decided 2026-10-27) Async preemption for fairness: no. The prologue and loop back-edge checks (D12.3) are the preemption mechanism, placed smartly: none in loops with a small known trip count, and once every 1024 iterations in other tight loops. Signal-based preemption could stop a goroutine inside C/C++ code (for example inside `malloc`, holding its lock) and deadlock the thread, and Go++ calls C/C++ everywhere. Async preemption is not planned.
