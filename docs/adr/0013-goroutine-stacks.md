# ADR-0013: Goroutine stacks (overflow detection and default size)

- **Status:** accepted
- **Date:** 2026-10-27
- **Plan reference:** P12 Q1 (task P12.02); P12 D12.1, D12.2, D12.3, D12.11.

## Context

Goroutine stacks in Go++ can't grow by copying the way Go's do: raw pointers into a stack are legal, and C/C++ frames (and the pointers C keeps) live on it (D12.1). Every stack is therefore a fixed reservation, with pages committed lazily by the OS. Two things were open:

1. **How an overflow is detected.** D12.2 listed guard pages (zero cost per call, catch C code too, but each one splits a mapping, and Linux's `vm.max_map_count` ≈ 65 k caps guarded stacks at about 32 k), compiler-inserted prologue checks (Go-style, no count limit, small cost per call, blind to C code), or a hybrid.
2. **The default reservation** (provisionally 256 KiB). For comparison, Go starts at 2–8 KiB and grows; Linux C threads get 8 MiB; Windows threads 1 MiB; musl threads 128 KiB.

The owner chose the hybrid and a 1 MiB default on 2026-10-27.

## Decision

1. **Hybrid overflow detection.**
   - **Every Go++ function checks its stack in the prologue:** `SP − frame size` against the goroutine's stack limit. It is the same compare that D12.3 already needs for the preemption flag (sysmon sets the limit to a value that always fails, and the slow path tells preemption from overflow, as in Go), so the overflow check adds no extra instruction on the fast path. Small leaf functions that fit in a fixed red zone skip it. An overflow panics with "goroutine stack overflow" and a trace.
   - **A guard page is added below each stack while the mapping budget allows it.** At startup the runtime reads `vm.max_map_count` and uses guard pages while its own mappings stay under half of it. Past that, new stacks get no guard page: Go++ code is still checked, and only a C overflow on those stacks goes undetected. Raising the sysctl brings the guard pages back. This is documented, with no runtime warning.
   - **C/C++ code:** `-fstack-clash-protection` stays on for C/C++ built by gopp, so a large C frame can't jump over a guard page. Go++ functions need no stack probes, because their prologue check already includes the frame size.
   - **Calls into C/C++ need headroom:** at a call into C or C++, the goroutine must have at least 64 KiB (provisional) left. Otherwise it panics with "goroutine stack overflow: not enough stack for a C call", before C runs.
   - The SIGSEGV handler on an alternate signal stack stays: it reports a guard-page hit as "goroutine stack overflow".
2. **The default reservation is 1 MiB on 64-bit targets.**
   - Only touched pages use real memory, so a small goroutine costs about the same as with 256 KiB; the larger ceiling costs only address space.
   - 1 MiB is the Windows thread default, so C/C++ libraries are already written to fit in it.
   - Stacks use `MADV_NOHUGEPAGE`, so transparent huge pages never commit 2 MiB at once. When a stack goes back to the pool, its touched pages beyond a small cached amount are released (`MADV_DONTNEED`).
   - `runtime.SetDefaultStack(n)` and `runtime.GoStack(n, f)` stay as they are. The main goroutine keeps the main thread's stack.
   - 32-bit targets get a smaller default, decided when such a target is added.

## Consequences

- No limit on the number of goroutines from the mapping count, and Go++ overflows are always caught.
- C overflows are caught while guard pages are in use (up to about 32 k goroutines with the default sysctl, which covers most programs), and the headroom rule stops most of them before C runs.
- The prologue check is shared with preemption (P12.03), so the cost is the one D12.3 already pays. P12.10 records the measured call overhead and the RSS per idle goroutine.
- The D12.11 target (100 k idle goroutines in ≤ 1 GiB RSS) no longer depends on the sysctl.
- The codegen (P04) emits the prologue check and the red-zone rule for leaf functions; the C-call lowering (P07/P08) emits the headroom check.

## Alternatives

- **Guard pages only.** Rejected: about 32 k goroutines unless every user raises a sysctl, while Go programs often run 100 k or more.
- **Prologue checks only.** Rejected: overflows in C code would go undetected, even for programs with few goroutines.
- **Go-style growable stacks** (copying). Not possible: pointers held by C and raw pointers into the stack can't be fixed up.
- **256 KiB default.** Rejected: the same real memory as 1 MiB for small goroutines, but less room for deep C/C++ code.
- **8 MiB default** (Linux C threads). Rejected: 1 M goroutines would reserve 8 TiB of address space.
