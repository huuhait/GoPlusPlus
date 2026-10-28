# P12 Concurrency: STATE

Legend: `[ ] not_started` · `[~] in_progress` · `[x] completed`
Phase status: `[ ] not_started`
Last updated: 2026-10-28 (ADR-0022 race-free concurrency)
Task definitions: [../TODO.md](../TODO.md) (section P12) · Design: [README.md](README.md)

## Tasks

- [ ] P12.01 Context switch
- [x] P12.02 Stack strategy ADR (owner decision, ADR-0013)
- [ ] P12.03 Scheduler
- [ ] P12.04 `go` lowering
- [ ] P12.05 Channels & select
- [ ] P12.06 sync & atomics
- [ ] P12.07 Netpoller
- [ ] P12.08 Threads, TLS and C calls
- [ ] P12.09 Sanitizers with goroutines
- [ ] P12.10 Benchmarks vs Go
- [ ] P12.11 Timers
- [ ] P12.12 Stop at exit
- [ ] P12.13 Race-free sync library (ADR-0022)

## Open questions

- [x] Q1 Default stack size; guard pages vs prologue checks (decided: hybrid, 1 MiB, ADR-0013)
- [x] Q2 io_uring for file I/O: decided, not in v1 (thread handoff like Go); maybe after v1 as a Linux-only speedup
- [x] Q4 Race-free concurrency details (decided 2026-10-28, ADR-0022; leftovers: a `Send` func type spelling in P04.21, guard surface in P12.13, `.go` mode in P04 Q3)
- [x] Q3 Async preemption: decided, no; smart compiler-placed checks only (D12.3: none in short known loops, every 1024 iterations in tight loops)

## Exit criteria

- [ ] M8: samples correct; stress tests pass; TSan clean
- [ ] Seeded data races in `unsafe` / C code detected by `-race` (in normal code they are compile errors, P04.21)
- [ ] Spin loop doesn't hang with one P; timers fire with all Ps busy
- [ ] D12.11 targets recorded and met (or ADR per gap)

## Decision changes

- 2026-10-27 (ADR-0006, P01.15 owner decisions): a channel and its buffered values are freed automatically (no `ch.Destroy()`); the marker that lets `sync.Scope` goroutines borrow (for example `@Scoped`) is designed here.

- 2026-10-28 (ADR-0022, owner decision): data races are compile errors in normal code. D12.6 and D12.7 rewritten: `Mutex[T]` / `RWMutex[T]` hold their data with auto-unlock guards, `Atomic[T]` seq_cst only, `sync.Scope` goroutines borrow locals with `Split`; plain `sync.Mutex` and `atomic.Add*` removed. New task P12.13 (library); the compile-time checks are P04.21. P12.06 narrowed.

## Notes / blockers

- Settle the P12.02 stack ADR before building the scheduler. The choice affects codegen (prologue checks), the rules for calling C, and the D12.11 goroutine target.
