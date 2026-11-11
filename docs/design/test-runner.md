# The fast test runner

Run tests compile a Go++ program, execute it, and compare `stdout`, `stderr`, and exit status against expected goldens. Compiling and spawning a standalone executable per test would pay process startup and full ELF/Mach-O/COFF linking overhead hundreds of times. Instead, `crates/gopp-cli` provides `gopp test-run` and `gopp test-exec` ([ADR-0001](../adr/0001-implementation-language.md)):

1. **`gopp test-run` (`batch_compile`):** Compiles many programs in parallel on a `rayon` (`1.12.0`) thread pool inside **one** `gopp` process, emitting GNU C23 and building lightweight shared modules (`.so` / `.dylib` / `.dll` via `clang -shared -fPIC -fuse-ld=lld` at `-opt=0`).
2. **`gopp test-exec` (`executor_pool`):** A small pool of long-lived worker processes loads each shared module via `libloading` (`0.9.0`: `dlopen` / `dlsym("gopp_entry")` / `dlclose`), captures `stdout`/`stderr`, resets process state when safe, and isolates crashes or `exit()` calls cleanly.

The runner is written for Linux. On `windows/amd64` the run tests use real executables until the runner has a Windows form ([ADR-0037](../adr/0037-v1-targets.md)).

## Consumers of the runner

| Consumer | How it uses the runner |
|---|---|
| Run test suite (`tests/run`) | Batches all test programs into one `gopp test-run` invocation; each test checks its recorded JSONL result. |
| Differential corpus (`P04.14`) | Feeds a manifest of 100+ programs and compares JSONL outputs against Go goldens. |
| `gopp test` (`P10` / `P14`, `_test.gopp` packages) | Compiles a test package once into a shared module and dispatches individual test functions through `ExecutorPool`. |
| Fuzzing campaigns ([fuzzing.md](fuzzing.md)) | Compiles and runs generated programs in bulk; the workers' limits and isolation are the sandbox for programs nobody has read. |
| Content-addressed build cache (`P06`) | Skips `batch_compile` on cache hits and passes cached runtime/`std` shared objects directly to `ExecutorPool`. |

## Architecture

```text
 JobSpec JSONL ──► [ batch_compile (rayon) ] ── .so modules ──► [ executor_pool ] ──► JobResult JSONL
   (manifest)        crates/gopp-cli                              N worker processes     (stdout or -o)
                     one gopp process,                            of `gopp test-exec`
                     parallel GIR + C23 + clang -shared           (dlopen / dlclose)
```

- **`batch_compile`:** Compiles each `JobSpec` to a position-independent shared module for the host using `select_pipeline` (`opt0`, [pipelines.md](pipelines.md)). Every job owns an independent arena, string interner, diagnostic sink, and GIR module — zero shared mutable state across `rayon` worker threads.
- **`executor_pool`:** Inspects each compiled shared module's undefined symbol table via `object` (`0.41.0`), dispatches it to a warm `gopp test-exec` worker over a Unix socket / pipe, and streams `JobResult` records as jobs finish so compilation and execution overlap.
- **`protocol`:** Versioned UTF-8 JSON Lines (`"v": 1`) shared by CLI, test harnesses, and `gopp test`.

## `gopp test-run` CLI

```text
gopp test-run [FILE|DIR ...] [-manifest FILE|-] [-o FILE] [-j N] [-executors N]
              [-timeout-ms T] [-max-output BYTES] [-work-dir DIR] [-merge-output] [-summary]
```

Positional arguments name single-file or directory programs; `-manifest` reads `JobSpec` JSONL lines (`-` for stdin). Exit status is `0` when every job produced a `JobResult` (regardless of whether tested programs exited non-zero or panicked), and `1` only on internal runner failure (`outcome: "internal"`). Hidden test flags (`-executor PATH`, `-share-executors`, `-startup-timeout-ms`, `-schedule forward|reverse|shuffle:SEED`) exercise edge cases deterministically.

## JSONL v1 protocol

Every message carries `"v": 1`; readers reject unknown major versions and ignore unknown additive fields.

**Input (`JobSpec`):**

```json
{"v":1,"id":"core-flow","inputs":["tests/run/core-flow.gopp"],"target":"host","timeout_ms":0}
```

`timeout_ms: 0` uses `-timeout-ms` (default `60000` ms). Optional `"merge_output": true` merges `stderr` into `stdout` in write order (`2>&1`).

**Output (`JobResult`):**

```json
{"v":1,"id":"core-flow","outcome":"exited","exit":0,"signal":0,"stdout":"<base64>","stderr":"<base64>","message":"","compile_ms":3.2,"run_ms":0.3,"restarted":false}
```

| `outcome` | Meaning | Key fields |
|---|---|---|
| `exited` | Program returned from `main`, called `exit(n)`, or panicked (status `2`) | `exit` |
| `signaled` | Terminated by signal `signal` (`SIGSEGV`, `SIGABRT`, `SIGXFSZ` past `-max-output`) | `signal` |
| `timeout` | Killed after `timeout_ms` | — |
| `compile-failed` | Front-end, GIR, or C23 compilation failed | `message` (rendered diagnostics) |
| `load-failed` | Shared module had unresolved `@Extern` symbols at `dlopen` | `message` (`undefined symbols: a b`, sorted) |
| `internal` | Executor binary failed to start | `message` |

## Worker isolation and process-state reset (`gopp test-exec`)

Each `gopp test-exec` worker starts with clean default signal dispositions, core dumps disabled, `stdin` bound to `/dev/null`, `RLIMIT_FSIZE` enforcing `-max-output` (default `256 MiB`, raising `SIGXFSZ` on runaway output), `PR_SET_PDEATHSIG` preventing orphan workers if the parent dies, and the worker's own 4 control descriptors (control socket, `/dev/null`, saved `stdout`, saved `stderr`) relocated to file descriptors `900..903` so user code sees `0`, `1`, `2` open and `3..899` free.

| Program behavior | Reported `outcome` | Worker lifecycle |
|---|---|---|
| Returns cleanly from `main` (state-free) | `exited` (`0`) | Reused after full state reset (`restarted: false`) |
| Calls `exit(n)` or panics (status `2`) | `exited` (`n`) | Worker exits via `_exit`; pool spawns replacement (`restarted: true`) |
| Crashes (`SIGSEGV`, `SIGABRT`, stack overflow) | `signaled` (`N`) | Reaped via `waitpid`; pool spawns replacement (`restarted: true`) |
| Exceeds `timeout_ms` | `timeout` | Killed by pool; replacement spawned (`restarted: true`) |
| Unresolved foreign symbol | `load-failed` | `dlopen` fails cleanly; worker stays warm (`restarted: false`) |
| Uses non-allowlisted libc symbol, leaves background threads, or hits 64-job cap | Program's own result | Worker recycled after completion (`restarted: true`) |

### State reset between reused runs

Before answering `"done"` after a normal return from `main`, `gopp test-exec` restores:

1. **Standard streams & file descriptors:** Flushes output, restores worker `stdout`/`stderr`, and closes every descriptor `>= 3` except its own four (`close_range` / `/proc/self/fd`).
2. **`errno`, timers, and signals:** Resets `errno = 0`, cancels `alarm`/`setitimer`, resets all catchable signals via `SIG_IGN` -> `SIG_DFL` (discarding pending signals), unblocks signal masks, and clears alternate signal stacks.
3. **Working directory, `umask`, environment, and `rlimit`:** Restores saved startup values (recycling the worker if a hard `rlimit` was irreversibly lowered).
4. **Thread count & module unload:** Verifies `/proc/self/task` has exactly 1 thread (recycling if extra threads exist) and unloads the program's `.so` via `dlclose`.

### Undefined-symbol allowlist (`object` `0.41.0`)

Because C libraries hold opaque internal state (`rand`, `strtok`, `setlocale`, `localtime`, `pthread_create`, stdio buffers), `executor_pool` inspects each compiled `.so`'s undefined symbols with `object` (`0.41.0`) before dispatch:

- **State-free allowlist (reuses warm worker):** `write`, `exit`, `_exit`, `abort`, `memcpy`, `memmove`, `memset`, `memcmp`, `bcmp`, `strlen`, `malloc`, `calloc`, `realloc`, `free`, runtime unwind symbols, and pure `libm` math functions (`sin`, `cos`, `sqrt`, `pow`, `floor`, `fma`, etc.).
- **Any other undefined symbol:** Marks the job `recycle: true` so the worker exits immediately after running it (`restarted: true`), guaranteeing zero cross-test state pollution.

### Known differences from standalone executables

- Stack and global addresses differ (`ASLR` / `dlopen` mapping base), so tests match pointer patterns rather than hardcoded addresses.
- `getpid()` and `argv[0]` belong to `gopp test-exec` when reused.
- Tests on `windows/amd64` ([ADR-0037](../adr/0037-v1-targets.md)) and linker-flag / determinism tests (`deterministic`, `link-flags`, `debian`) always build and execute real standalone binaries (`--real` / `GOPP_FAST_RUN=0`).

## Batch CLI mode (`-batch`)

For harnesses that run `gopp parse`, `gopp fmt`, or standalone `gopp build` across hundreds of files (such as the full `-opt` equivalence matrix), `-batch` processes a stream of files or JSONL build specs inside a single `gopp` process using `rayon`, emitting framed `gopp-batch <exit> <stdout_len> <stderr_len> <id>\n` records with automatic per-item fallback if a batch ever times out or traps.
