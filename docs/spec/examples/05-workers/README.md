# 05 workers

Three worker goroutines read jobs from a channel and send results back. The results are sorted by job ID, so the output is deterministic. Run: `go run main.go`.

## Spec rules exercised

- [CON-1, CON-3, CON-4](../../12-concurrency.md): `jobs <- Job{...}` moves the job in; `range jobs` and `range results` move values out; closures used with `go` share their variables.
- [CON-2](../../12-concurrency.md): `go work(jobs, results, &wg)` would be rejected (raw pointer to a local), so the program uses closures.
- [CON-9](../../12-concurrency.md), [OWN-R10](../../04-ownership.md): `sync.WaitGroup` is `@Pinned`; it lives in a shared cell and is never moved.
- [CLO-2, CLO-3](../../11-closures.md): the two `go func(){...}()` closures escape, so `wg`, `jobs` and `results` are in cells.
- [RNG-2](../../08-slices-and-range.md): `range move(words)` moves each string out; `words` is the zero value afterwards.
- [OWN-R3, OWN-R3a, OWN-R4](../../04-ownership.md): `Text: move(job.Text)` (a field needs `move(x)` or `x.Clone()`); `Job{..., Text: w}` moves `w`; `all = append(all, r)` moves `r` and `all`.
- [PAR-1, PAR-7](../../05-parameters.md): `Result` parameters of the sort callback are borrowed; channels pass as shared handles.

## Differences from the Go version

- Go++ needs `move(job.Text)`; Go copies the string header.
- Go copies everything through channels (sharing the bytes); Go++ moves, so no copy is made.
- Go++ ranges with `move(words)` to hand the words over. Go ranges over `words` normally.
- Output is identical.
