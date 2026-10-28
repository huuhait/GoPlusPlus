# P12 Concurrency: STATE

Legend: `[ ] not_started` · `[~] in_progress` · `[x] completed`
Phase status: `[ ] not_started`
Last updated: 2026-10-27 (Q1–Q3 decided)
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

## Open questions

- [x] Q1 Default stack size; guard pages vs prologue checks (decided: hybrid, 1 MiB, ADR-0013)
- [x] Q2 io_uring for file I/O: decided, not in v1 (thread handoff like Go); maybe after v1 as a Linux-only speedup
- [x] Q3 Async preemption: decided, no; smart compiler-placed checks only (D12.3: none in short known loops, every 1024 iterations in tight loops)

## Exit criteria

- [ ] M8: samples correct; stress tests pass; TSan clean
- [ ] Seeded data races detected by `-race`
- [ ] Spin loop doesn't hang with one P; timers fire with all Ps busy
- [ ] D12.11 targets recorded and met (or ADR per gap)

## Decision changes

- 2026-10-27 (ADR-0006, P01.15 owner decisions): a channel and its buffered values are freed automatically (no `ch.Destroy()`); the marker that lets `sync.Scope` goroutines borrow (for example `@Scoped`) is designed here.

## Notes / blockers

- Settle the P12.02 stack ADR before building the scheduler. The choice affects codegen (prologue checks), the rules for calling C, and the D12.11 goroutine target.
