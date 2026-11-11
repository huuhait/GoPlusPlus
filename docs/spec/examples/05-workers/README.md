# 05 workers

Three worker goroutines read jobs from a channel and send results back. The results are sorted by job ID, so the output is deterministic. Run: `go run main.go`.

## Spec rules exercised

- [CON-1, CON-3, CON-4](../../12-concurrency.md): `jobs <- Job{...}` transfers the job into the channel; `range jobs` and `range results` receive values out; closures used with `go` share their captured variables in atomic cells.
- [CON-2](../../12-concurrency.md): `go work(jobs, results, &wg)` would be rejected (raw pointer to a local), so the program uses closures.
- [CON-9](../../12-concurrency.md), [OWN-R10](../../04-ownership.md): `sync.WaitGroup` is `@Pinned`; it lives in a shared cell and is never moved.
- [CON-12, CON-13, CON-14](../../12-concurrency.md): the closures capture only `wg`, `jobs` and `results`, which are `Sync` (a `WaitGroup` and channels), so they are shared with no race. `Job` and `Result` are `Send` (`string` bytes are immutable, [ADR-0032](../../../adr/0032-shared-buffers-and-borrow-engine.md)) and transfer across channels cleanly.
- [CLO-2, CLO-3](../../11-closures.md): the two `go func(){...}()` closures escape, so `wg`, `jobs` and `results` live in shared cells.
- [OWN-C2, OWN-R4](../../04-ownership.md): `job.Text` and `r` are on their last uses in the loop bodies, so `Result{ID: job.ID, Text: job.Text, Sum: sum}` and `all = append(all, r)` auto-move with zero retain/release.
- [PAR-1](../../05-parameters.md): `Result` parameters of the sort callback and `chan` parameters of `work` are lent at 0 retain/release.

## Differences from the Go version

- None in syntax: `main.go` and `main.gopp` use identical Go code (only comments differ). Output is identical.
