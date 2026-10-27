# P12 Concurrency: STATE

Legend: `[ ] not_started` · `[~] in_progress` · `[x] completed`
Phase status: `[ ] not_started`
Last updated: 2026-10-27
Task definitions: [../TODO.md](../TODO.md) (section P12) · Design: [README.md](README.md)

## Tasks

- [ ] P12.01 Context switch
- [ ] P12.02 Stack strategy ADR
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

- [ ] Q1 Default stack size; guard pages vs prologue checks (P12.02)
- [ ] Q2 io_uring for file I/O (after v1)
- [ ] Q3 Async preemption (after v1, only if needed)

## Exit criteria

- [ ] M8: samples correct; stress tests pass; TSan clean
- [ ] Seeded data races detected by `-race`
- [ ] Spin loop doesn't hang with one P; timers fire with all Ps busy
- [ ] D12.11 targets recorded and met (or ADR per gap)

## Decision changes

- 2026-10-27 (ADR-0006, P01.15 owner decisions): a channel and its buffered values are freed automatically (no `ch.Destroy()`); the marker that lets `sync.Scope` goroutines borrow (for example `@Scoped`) is designed here.

## Notes / blockers

- Settle the P12.02 stack ADR before building the scheduler. The choice affects codegen (prologue checks), the rules for calling C, and the D12.11 goroutine target.
