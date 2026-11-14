# The fast test runner

Run tests compile a Go++ program, execute it, and compare `stdout`, `stderr`, and exit status against expected goldens. Compiling and spawning a standalone executable per test would pay process startup and full ELF/Mach-O/COFF linking overhead hundreds of times. Instead, `crates/gopp-cli` provides `gopp test-run` and `gopp test-exec` ([ADR-0001](../adr/0001-implementation-language.md)):

1. **`gopp test-run` (`batch_compile`):** Compiles many programs in parallel on a `rayon` (`1.12.0`) thread pool inside **one** `gopp` process, emitting GNU C23 and building lightweight shared modules (`.so` via `clang -shared -fPIC -fuse-ld=lld` at `-O0`). A module is not linked with the runtime: it finds the functions of the runtime in the process that loads it.
2. **`gopp test-exec` (`executor_pool`):** A small pool of long-lived worker processes, each of which loads the shared runtime once (`libgopp_rt.so`, [runtime.md](runtime.md)) and then each shared module via `libloading` (`0.9.0`: `dlopen` / `dlsym("gopp_entry")` / `dlclose`), captures `stdout`/`stderr`, resets process state when safe, and isolates crashes or `exit()` calls cleanly.

The code is `crates/gopp-cli/src/runner.rs` (the command, the building, real executables), `runner/protocol.rs`, `runner/pool.rs` and `runner/exec.rs` (the worker, the one module of `gopp` that asks the system directly).

The runner is written for Linux. On `windows/amd64`, and with `-real` or `GOPP_FAST_RUN=0` anywhere, each program is an executable of its own, linked with the objects of the runtime and run as a process ([ADR-0037](../adr/0037-v1-targets.md)); the results have the same lines.

## Consumers of the runner

| Consumer | How it uses the runner |
|---|---|
| Run test suite (`tests/run`) | Batches all test programs into one `gopp test-run` invocation; each test checks its recorded JSONL result. |
| Differential corpus (`tests/corpus`) | Feeds a manifest of its programs, which are Go programs too, and compares what each wrote and how it ended with the file that Go wrote for it ([differences-from-go.md](differences-from-go.md)). |
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

- **`batch_compile`:** Compiles each `JobSpec` to a position-independent shared module for the host, at `-O0` unless `-opt` says another level. Every job owns an independent arena, string interner, diagnostic sink, and GIR module — zero shared mutable state across `rayon` worker threads. The entry of a module is `int gopp_entry(void)`, which the emitter writes in the place of `main` (`Entry::Module`).
- **`executor_pool`:** Inspects each compiled shared module's undefined symbol table via `object` (`0.41.0`), dispatches it to a warm `gopp test-exec` worker over a Unix socket, and streams `JobResult` records as jobs finish so compilation and execution overlap. Each executor thread keeps one worker and starts a new one when the last program did not leave it whole. The socket is the standard input that the worker is started with; requests and answers are lines of JSON.
- **A job of C:** a job whose one input is a `.c` file is compiled as it is, after the header of the runtime, and must define `gopp_entry` itself. The tests of the runner and of the runtime use it.
- **C beside a program:** the inputs of a job that end in `.c` and stand beside a Go++ program are compiled by the same Clang, with the same flags, and linked with it. The run tests that cross to C use it (`tests/run/cabi_*.gopp`, each with a `.c` file of its name).
- **`protocol`:** Versioned UTF-8 JSON Lines (`"v": 1`) shared by CLI, test harnesses, and `gopp test`.

## `gopp test-run` CLI

```text
gopp test-run [FILE|DIR ...] [-manifest FILE|-] [-o FILE] [-j N] [-executors N]
              [-timeout-ms T] [-max-output BYTES] [-work-dir DIR] [-merge-output] [-summary]
              [-real] [-opt=0|1|2|3|s|z] [-debug] [-panic=unwind|abort]
```

`-opt` says how much Clang optimizes the programs of the run, and the runtime that they are linked with; it is `0` unless it is given. `-debug` builds the programs with what a debugger reads, as `gopp build` does ([debug-info.md](debug-info.md)). `-panic=abort` builds them so that a panic ends a program at once, as `gopp build -panic=abort` does (PAN-7). Positional arguments name single-file or directory programs; `-manifest` reads `JobSpec` JSONL lines (`-` for stdin). Exit status is `0` when every job produced a `JobResult` (regardless of whether tested programs exited non-zero or panicked), and `1` only on internal runner failure (`outcome: "internal"`). A program of the command line has the name of its file as its `id`. `-work-dir` keeps what was built and written: `DIR/<number of the job>/` has `unit.c`, `module.so` (or `program`), `stdout` and `stderr`. `-summary` prints the counts and the times to standard error. Hidden test flags exercise edge cases deterministically: `-executor PATH` (the program started as the worker), `-startup-timeout-ms`, and `-schedule forward|reverse|shuffle:SEED`, which builds every job first and then gives them to the executors in that order.

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
| `internal` | Executor binary failed to start, or did not get ready in time | `message` |

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

- **State-free allowlist (reuses warm worker):** every function of the runtime (`_gopp_...`, whose state `_gopp_rt_init` clears), `write`, `exit`, `_exit`, `abort`, `memcpy`, `memmove`, `memset`, `memcmp`, `bcmp`, `strlen`, `malloc`, `calloc`, `realloc`, `free`, runtime unwind symbols, and pure `libm` math functions (`sin`, `cos`, `sqrt`, `pow`, `floor`, `fma`, etc.). A weak name that may stay undefined is not counted.
- **Any other undefined symbol:** Marks the job `recycle: true` so the worker exits immediately after running it (`restarted: true`), guaranteeing zero cross-test state pollution.
- **`load-failed`:** when a module does not load, the worker asks for each of its undefined names whether something defines it, and the message names those that nothing does, sorted.

### Known differences from standalone executables

- Stack and global addresses differ (`ASLR` / `dlopen` mapping base), so tests match pointer patterns rather than hardcoded addresses.
- `getpid()` and `argv[0]` belong to `gopp test-exec` when reused.
- A real executable is a process of its own every time, so its result always has `restarted: true`; without Linux there is no limit on what it writes.
- Tests on `windows/amd64` ([ADR-0037](../adr/0037-v1-targets.md)) and linker-flag / determinism tests (`deterministic`, `link-flags`, `debian`) always build and execute real standalone binaries (`--real` / `GOPP_FAST_RUN=0`).

## The run tests (`tests/run`)

A program of the suite is a `.gopp` file in `tests/run`. Beside it, `NAME.out` has what it writes, the standard error stream in the standard output in the order of the writes (`merge_output`); a first line `// exit: N` in the program says the status that it ends with, 0 without one. The offsets of a trace are written `+0x?`. `crates/gopp-cli/tests/run.rs` gives every program to one `gopp test-run` and compares each result; `GOPP_RUN_UPDATE=1` writes the `.out` files anew. A program with a file `NAME.abort.out` beside it ends with the status 2 when it is built with `-panic=abort` and writes what that file says; a long test builds every program that stops no panic in that mode too, and each writes the same as without it. The suite uses the fast way by default and real executables with `GOPP_FAST_RUN=0`.

## Tests and times

`crates/gopp-cli/tests/runner.rs` has each row of the two tables above: every outcome; that one worker runs program after program and each finds the directory, the mask and the descriptors as the first found them (programs of C that ask the system without the C library, so that nothing marks them); that a worker ends with a program that names a function with state, after a program that ends it, and after 64 programs; that the results are the same forward, reversed, shuffled and many at a time (the long test `many_orders_change_no_result` has 38 more orders); that real executables end the same way; and what the runner says when it cannot run.

96 small programs, built and run (`-j N -executors N`): 9.95 s on one thread, 4.88 on two, 2.48 on four, 1.26 on eight, 0.25 on all 192; as real executables on all, 0.42 s. One program costs about 100 ms of Clang and LLD, which is nearly all of it.

## Batch CLI mode (`-batch`)

For harnesses that run `gopp parse`, `gopp fmt`, or standalone `gopp build` across hundreds of files (such as the full `-opt` equivalence matrix), `-batch` processes a stream of files or JSONL build specs inside a single `gopp` process using `rayon`, emitting framed `gopp-batch <exit> <stdout_len> <stderr_len> <id>\n` records with automatic per-item fallback if a batch ever times out or traps.

`gopp parse -batch [-emit=ast|json]` is the first such command. It reads file names from standard input, one per line, parses them on all cores, and writes the records in input order: the header line, then `stdout_len` bytes of what `gopp parse <file>` prints to standard output, then `stderr_len` bytes of what it prints to standard error. `<id>` is the file name as given. The exit status of the batch is 1 when any record's is.
