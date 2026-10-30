# The fast test runner

Status: P04.19. Decision: P04 D4.14 (a fast test runner), P00 D0.13 (the dev-loop budgets). Code: `compiler/include/gopp/TestRun/`, `compiler/lib/TestRun/`, `gopp test-run` and `gopp test-exec` in `compiler/tools/gopp/gopp.cpp`; lit side `tests/lit.cfg.py`, `tests/lit_gopp_format.py`, `tests/run/Inputs/runlib.py`.

## What it is for

Run tests compile a Go++ program, run it and compare stdout, stderr and the exit status with Go's. As real binaries each one costs a `gopp build` process, a linker process and the program's own process. The runner compiles many programs in ONE process and runs them without linking, in a few long-lived executor processes with an ORC JIT.

It is built to be the engine of three later users, not only of today's lit tests:

| User | What it takes from the runner |
|---|---|
| lit run tests (now) | `gopp test-run` over the programs of all run tests; each test reads its program's result |
| P04.14 differential corpus | one manifest of 100+ programs, results as JSON lines, compared with Go's goldens by the harness |
| `gopp test` (P10/P14: `_test.gopp` packages) | `ExecutorPool` and the protocol; the compile side compiles a package with its test functions once and the executor calls one test per request |
| P06 build cache, cached runtime and std | the compile side is `BatchCompile.h`; a cache hit never calls it. Cached objects of the runtime and std (P05.01 "also loadable by the JIT test runner") are added to the executor's link order |

## Three parts that know each other only through the protocol

```
 JobSpec lines ──► [ batch compile driver ] ── object files ──► [ executor pool ] ──► JobResult lines
   (manifest)       BatchCompile.h                               ExecutorPool.h        (stdout, -o file,
                    many threads, one process                    N processes of        a reader in lit
                                                                 `gopp test-exec`      or `gopp test`)
                                      Protocol.h: the lines
                                      Runner.h: joins the two (this is `gopp test-run`)
```

- **`BatchCompile.h`** compiles a `JobSpec` to an object file for the host: the steps of `gopp build -emit=obj` with no command-line state, so any number run at once. Every compile owns its source manager, diagnostics, type context and LLVM context; nothing mutable is shared. `compileBatch` runs them on a thread pool. It knows nothing about running.
- **`ExecutorPool.h`** takes object files and returns `JobResult`s. It knows nothing about the compiler.
- **`Protocol.h`** is the only thing the two share, and the only thing a reader outside needs.
- **`Runner.h`** (`runBatch`) hands each finished object to the pool at once, so compiling and running overlap, and the executors start while the first programs compile.

Other wirings use the parts directly: P06's cache calls the pool with an object it found in the cache, and `gopp test` keeps one compiled package and sends many requests to it.

## `gopp test-run`

```
gopp test-run [FILE|DIR ...] [-manifest FILE|-] [-o FILE] [-j N] [-executors N]
              [-timeout-ms T] [-max-output BYTES] [-work-dir DIR] [-merge-output] [-summary]
```

Each positional argument is one program (its id is the path); `-manifest` gives jobs as JSON lines. Results go to standard output or `-o`, one line per program as it ends. `-summary` prints the count of each outcome and of the executor processes started. The exit status is 0 when every program got a result, whatever the programs did; 1 when the runner itself failed, including a program that got no executor (outcome `internal`, also printed on standard error). Hidden test hooks: `-executor PATH` (start another program as the executor), `-share-executors` (also share an executor between programs that keep C library state, to test what the executor puts back), `-startup-timeout-ms`, `-schedule forward|reverse|shuffle:SEED` (the order the programs are compiled and run in).

## The result protocol

JSON lines, UTF-8, one message per line, every message has `"v": 1`. A reader refuses another version. New fields may be added without a version bump (readers ignore fields they do not know); a field that changes meaning bumps the version.

Job (input of `gopp test-run -manifest FILE`, `-` is standard input):

```
{"v":1,"id":"core-flow","inputs":["tests/run/core-flow.test"],"target":"host","timeout_ms":0}
```

`inputs` are source files or one directory, as `gopp build` takes them. `id` is unique in a run. `timeout_ms` 0 means the runner's `-timeout-ms` (default 60000). `"merge_output": true` (optional) puts standard error into the same file as standard output, in the order the program wrote them, as a shell's `2>&1`; the result then has both in `stdout` (`-merge-output` sets it for every job).

Result (output, one line per job, in the order the jobs end):

```
{"v":1,"id":"core-flow","outcome":"exited","exit":0,"signal":0,
 "stdout":"<base64>","stderr":"<base64>","message":"","compile_ms":3.2,"run_ms":0.4,"restarted":false}
```

| `outcome` | Meaning | Fields set |
|---|---|---|
| `exited` | the program ended with status `exit` (a return from `main`, `exit(n)`, a panic's 2) | `exit` |
| `signaled` | killed by signal `signal` (SIGSEGV from `unsafe` or a stack overflow, SIGABRT from `abort`, a signal it sent itself, SIGXFSZ past the output limit) | `signal` |
| `timeout` | the runner killed it after `timeout_ms` | |
| `compile-failed` | the program does not compile; `message` holds the diagnostics | `message` |
| `load-failed` | the object would not link in the JIT (an `@Extern` function that does not exist); `message` names the missing symbols, sorted (`undefined symbols: a b`) | `message` |
| `internal` | the runner failed (no executor would start) | `message` |

`stdout` and `stderr` are base64 so any bytes survive. `restarted` is true when the executor process ended with this program (an `exit`, a crash, a time-out, or a program that may keep state the executor cannot put back), so a reader can tell the programs that cost a process from those that did not.

Between the pool and one executor the same style is used over a socket: `{"cmd":"run","id","object","stdout","stderr","max_output"}` (`stderr` empty: merged) and `{"cmd":"quit"}` one way; `{"event":"ready"}` and `{"event":"done","id","loaded","exit","error","recycle"}` the other. The executor opens the output files itself and the pool reads them afterwards.

## The executors

`gopp test-exec` (started by the pool; not for people) is a process with an LLJIT. For each request it makes a fresh `JITDylib`, resolves the program's undefined symbols against its own process (libc: `write`, `exit`, `abort`, `memcpy`, the libm functions), adds the object, looks up `main`, calls it on the main thread with standard output and standard error going to the job's files, then removes the dylib (the program's code and data are freed; the executor stays if the program was state-free).

What ends an executor, and what the program's result is:

| The program | Result | Executor |
|---|---|---|
| returns from `main` | `exited` with main's status | lives on (puts back the process state, below) |
| calls `exit(n)`, panics (exit 2) | `exited` n | ends, replaced |
| crashes: SIGSEGV, SIGABRT, a stack overflow, any signal | `signaled` N | ends, replaced |
| runs past its time limit | `timeout` | killed, replaced |
| writes more than `-max-output` bytes (default 256 MiB) to one stream | `signaled` 25 (SIGXFSZ) | ends, replaced |
| calls a C function that does not exist | `load-failed` | lives on |
| is not state-free (an undefined symbol off the allowlist), changes process state that cannot be put back, or leaves a thread | its own result | replaced after it (`restarted`) |
| (no executor starts: missing, ends at once, never ready) | `internal`, the same text for every program | none |

- **Crash, `exit`, panic.** The pool sees the end of the socket, asks the operating system how the process ended (`waitpid`) and reports that as the program's result, with the output files' bytes. The next job starts a new executor.
- **`exit` in the JIT** is not libc's: it flushes the C streams and calls `_exit`, so the executor's own destructors (and, in a sanitizer build, the leak check) do not run and change the status. The executor itself also leaves with `_exit`, so nothing a program registered can run after its code is gone.
- **Like a binary's process.** `test-exec` starts before `InitLLVM`, so no crash handler prints a stack trace into the captured stderr; every signal has its default action, none is blocked, and there is no alternate signal stack. Standard input is `/dev/null`. The executor's own descriptors (the socket, a `/dev/null`, its own stdout and stderr) are moved to 900 and up, so a program finds 0, 1 and 2 open and 3 free, as in a binary started by a test. Core files are off (a crash must not dump the whole executor). The output limit is `RLIMIT_FSIZE` during the program: a program that prints forever ends with SIGXFSZ instead of filling the disk and the runner's memory.
- **Link errors** do not depend on scheduling: ORC's own message names the program's JITDylib (`job12`: how many programs the executor ran before) and prints hash-ordered sets, so the executor rebuilds it from the sorted missing symbols.
- **Recycling.** An executor is replaced after 64 programs, so memory a leaking program left (owned values leak until P04.09) does not pile up; and after every program that is not state-free (see "One process, many programs"). The count of processes started is the work the stress test counts.
- **Start failures.** Every wait has a limit: an executor that has not said "ready" within 20 s did not start. After three failed starts in a row the pool starts no more, and every job that needs an executor gets the first failure's text at once (`the executor does not start: cannot start X: No such file or directory`, `... it ended before it was ready (exit status 1)`, `... it was not ready after N ms`).
- **Work directory.** Objects and output files live in `-work-dir DIR` (kept; the caller removes it) or in a temporary directory that `test-run` removes at the end, and also when it gets SIGTERM, SIGINT or SIGHUP (it ends with 128 + the signal). SIGKILL cannot be caught: a caller that may be killed passes its own `-work-dir` (the lit batch does).
- **Orphans.** `PR_SET_PDEATHSIG` kills an executor when the pool's process dies.
- **How many.** The default is a quarter of the cores, 2 to 16, and never more than there are programs: a program runs for milliseconds, and each executor costs a process start (about 25 ms of CPU, mostly mapping the shared LLVM library).
- **Why a process and not a thread:** a program may do anything a program can do. This is the model of LLVM's `llvm-jitlink-executor` and ORC's `ExecutorProcessControl`; here the compile happens in the driver and only the finished object travels, so the executor needs no more than LLJIT and a socket.

### One process, many programs

A program that returns from `main` leaves its executor to the next program, so the next one must not see what it did to the process (manager decision M2). The executor takes the state of the process when it starts, and after every program that returns, before it answers, puts back:

| State | How |
|---|---|
| standard input, output and error (closed, `dup2`ed over) | 0 is `/dev/null` and 1 and 2 the job's files anew for every program; afterwards the executor's own again |
| descriptors the program opened and did not close | every descriptor from 3 up except the executor's own four is closed (`close_range` over the gaps; `/proc/self/fd` on a kernel without it), also the ones above 900 |
| `errno` | set to 0 (a libm function that failed leaves it set) |
| threads | the process must have exactly one (`/proc/self/task`) after the program; if not, the executor ends (`recycle`) and the program's code is left in place |
| signal dispositions (`SIG_IGN`, handlers), the mask, pending signals, the alternate stack | every signal set to `SIG_IGN` then `SIG_DFL` (that discards a pending one, as the end of the program's process would), nothing blocked, no alternate stack |
| interval timers (`alarm`, `setitimer`) | cancelled, before the signals are reset |
| umask, working directory | set back |
| environment (`setenv`, `putenv`, `unsetenv`, writes through `getenv`) | cleared and set again from fresh copies |
| resource limits (`setrlimit`) | set back; a lowered hard limit cannot be raised again, so the executor asks for a new process (`recycle`) |
| the program's code and globals | its JITDylib is removed; the next program loads its own |
| a child the program forked that returns from `main` | ends there with main's status and never talks to the pool |

A program that closes the executor's socket cannot be answered; its executor then leaves with main's status, which the pool reports as the program's (the same as `exit`). `tests/run/runner-reuse.test` runs each case and then a program that looks at that state, in ONE executor, and compares both with their real binaries.

**The rule: only a program that touches no hidden state shares an executor.** The list of things the executor puts back is not complete, and a C library keeps state nobody can list (the `rand` generator, `strtok`, `localtime` and the time zone, `setlocale`, `setvbuf`, `getopt`, `mallopt`, `clock`, POSIX timers, threads, ...). So the pool decides per program, from the compiled object, and does not try to reset such state:

- It reads the object's undefined symbols (what the program takes from the C library), no round trip to the executor.
- If every one is on this allowlist, the program is state-free and the next program may use the same executor: `write`, `exit`, `_exit`, `abort`, `memcpy`, `memmove`, `memset`, `memcmp`, `bcmp`, `strlen`, `malloc`, `calloc`, `realloc`, `free`, and the libm functions (`sin`, `sqrt`, `pow`, `floor`, ..., each also with the `f` suffix; the list is in `ExecutorPool.cpp`). The list was made from `nm -u` over the run corpus (it needs `write`, `exit`, `abort`, `memmove`, `memset`) and the functions the runtime and plain C code can need.
- If any other symbol is there (`rand`, `getenv`, `getpid`, `pthread_create`, `stdout`, ...), or the object cannot be read, the program is marked `recycle`: its executor ends with it (`restarted` is true), and the next program gets a new one.
- A program that does not load (`load-failed`) never ran, so it costs no executor.
- Independently of the symbols, an executor that has more than one thread after a program (it started one by a path the symbols did not show) ends too.

A new symbol is added to the allowlist only when it keeps no state of the process (a pure function, or one whose only state the executor puts back). The cost of a program off the list is one process start (about 25 ms of CPU); `compare-runner.py --list` shows how many programs of the corpus pay it.

What the executor still puts back (the table above) is a second line of defence for state a state-free program cannot change, and what `tests/run/runner-reuse.test` tests: with the TEST ONLY switch `-share-executors` the pool shares executors between all programs, so that test can run a program that changes the state, then one that looks at it, in ONE executor. `tests/run/runner-recycle.test` tests the rule: `rand-a` then `rand-b` equal their real binaries (two executors), a row of pure programs shares one (exact start count), a program that starts a thread ends its executor even when sharing is forced.

Not put back, so the rule is what protects against them: C stdio state (`setvbuf`, `fclose` of a standard stream, `freopen`), the locale, POSIX timers (`timer_create`), threads, child processes that keep running, process attributes (`prctl`, session and group, priority, CPU affinity, user and group ids), writing to or replacing the executor's descriptors at 900 and up, and memory corrupted through `unsafe` without a crash (a later program may crash instead; recycling bounds it). `atexit`: not resolvable in the JIT today (it lives in `libc_nonshared.a`, not in the C library's dynamic symbols), so such a program does not load; Go++ cannot pass a function to C before P04.18 anyway.

### The executor's four descriptors

The executor holds four descriptors for itself, at the lowest free numbers from 900 (or 100, or 10 if the limit is lower): the socket to the pool, a `/dev/null` (standard input of every program), and copies of its own standard output and standard error. After every program all others from 3 up are closed, those above 900 too. A program has the same descriptors as a binary except these four, so with a descriptor limit of 1024 a program can open 1017 files in an executor and 1021 as a binary (`tests/run/runner-fds.test`: a program opens more than 900 files and leaves them open; a second program opens the same number). With no limit a program that opens files until it fails sees four numbers skipped at 900 to 903.

### Where it differs from a real binary

These are the known differences. None changes what the run tests check (stdout, stderr, status, the first line of a panic).

- Addresses: the stack and the program's data are in other places. A test must not print an address and compare it (`core-ptr-print` matches a pattern).
- `argv[0]`, the auxiliary vector, static constructors of the object: not set up (the object has none today). The process id is the executor's, the same for every program it runs.
- Resource limits are the executor's: core files off, and the file size limit while the program runs.
- The working directory is the one `gopp test-run` was started in (for the lit batch, lit's), not the test's.
- A link error is a `load-failed` result when the program runs, where a real build fails to link (`compile-failed` in `runlib.py`'s real mode).
- JIT code is not PIE-loaded by the system loader: a test about how the binary is linked or loaded must be real.

## Lit integration

`tests/run` tests call the run scripts with `--src` instead of building a binary first:

```
// RUN: %python %S/Inputs/run-golden.py --src %s %S/golden/core-flow.out 0
```

`run-golden.py`, `run-expect.py`, `run-panics.py` and `run-output.py` (stdout for FileCheck) share `Inputs/runlib.py`. Goldens and expectations are unchanged.

1. **Batch before the tests.** `GoppTestFormat` (`tests/lit_gopp_format.py`) reads the RUN lines of `tests/run` at discovery (lit's own substitutions applied, so `%if` and `%S` are exact), asks `runlib.plan` which programs the run scripts will run, and runs ALL of them in one `gopp test-run` on every core. Each result is saved as `<key>.json` in a new directory under `build/tests/fastrun/` and the tests get `GOPP_FASTRUN_DIR`. Only the tests that will run are batched (normal tests, or the `.long.test` ones under `check-long`).
2. **The key** is a hash of the sources (their paths, in order, and their bytes; a directory's files), the size and time stamp of the `gopp` binary and the protocol version. A result is never used for another program or compiler: a source changed after the batch ran gives another key, and that test runs its program alone (`runner-batch.test`; each part of the key both ways in `runner-key.test`).
3. **A test run alone** (`lit one.test`) finds no result and runs `gopp test-run` for its own program: one compile, one executor, no link.
4. **Fallback.** If the batch fails in any way (an exit status other than 0, a `gopp` that cannot be started, results that are not results of this batch), lit prints a warning, nothing of the batch is used, and every test runs its program alone (`runner-batch-fallback.test` forces each failure with the TEST ONLY lit parameter `fastrun_batch_gopp`).
5. **How the batch is run.** The environment is `config.environment`, as for a real binary, not lit's own. The batch gets `-j` (compile threads) and `-executors` (a quarter of it, 2 to 16) from lit's own `-j`/`--workers` (read from lit's command line and `LIT_OPTS` with lit's parser: lit gives discovery no worker count), `-work-dir` inside its output directory (removed when the batch ends, so a killed `gopp` leaves nothing in `/tmp`), `-max-output 16 MiB` per stream of a program (past it SIGXFSZ, a failed test: no test prints that much), and an outer limit of 300 s. It runs at discovery, so also for `--filter` (all programs of `tests/run`, about 0.2 s of wall time); `--show-tests` and `--show-suites` skip it. The cleanup of old batch directories tolerates a second lit run on the same build directory (`runner-parallel-lit.test`).
6. **Time accounting.** The batch runs outside any test, so `tools/check-suite-time.py` adds the batch's CPU time (`GOPP_FASTRUN_STATS`) to the sum of test times; the gate cannot be satisfied by moving work into the batch.

Real binaries, by switch:

| Switch | Effect |
|---|---|
| `--real` in a RUN line | that one run is a real binary (`gopp build` + exec) |
| `lit -Dreal_binaries=1`, `GOPP_FAST_RUN=0`, or `ninja check-run-real` | all run tests use real binaries |
| `--qemu SYSROOT` or the old `-- BUILD...` form of `run-panics.py` | another target: real, under QEMU |

`GOPP_RUNLIB_TRACE=1` prints how each program was run (`tests/run/runner-modes.test` uses it to prove both modes).

Kept as real binaries, because the test is about the binary: `hello.test` (M0 end to end), `debian.test`, `link-flags.test`, `deterministic.test`, `exit-code.test` (exit path of a real process), every arm64 test (QEMU), the Debian container test.

## Tests

- `tests/unit/TestRun/TestRunTest.cpp`: protocol (every byte value, the new fields, bad version, bad fields), batch compile (diagnostics, 48 threads give the same bytes), the pool (output, `exit`, panic, SIGSEGV, SIGABRT, time-out, link failure with its exact text, compile error each end only their program and the next runs; an executor that does not start gives every job the same `internal` result), exact output bytes, and the stress test that counts executor processes started (crashes and recycling, with a control run of twice the length).
- `tests/run/runner-crash.test`: eight programs on two executors with every kind of ending, FileCheck on the sorted results; and seven on ONE executor, where the number of processes started is exact (5).
- `runner-ends.test`, `runner-streams.test`, `runner-limits.test`: the ways a program ends, output without a final newline, megabytes of it and both streams in turn (separate and merged), the time limit and the output limit; each result compared with a real binary built from the same file (`Inputs/runner-check.py`, programs in `Inputs/runner` with their `// EXPECT:` line).
- `runner-failures.test`: executors that do not start (three ways), link errors and compile errors (`Inputs/runner-scenarios.py`). `runner-determinism.test`: the same bytes for four schedules (threads, executors, order).
- `runner-reuse.test`: the state a program changes, followed by a program that looks at it, in one executor, each compared with its real binary; the number of processes proves the executor was reused.
- `runner-modes.test`: fast and real give the same verdicts; both fail on a wrong golden, a wrong status, a program that does not compile.
- `runner-batch.test`, `runner-batch-fallback.test`, `runner-key.test`: the lit batch in a nested lit (`Inputs/batch-suite`): results taken from the batch, a changed source not, a failed batch falls back, the batch has the tests' environment and not lit's; the key both ways. `runner-batch-args.test`: the batch's `-j`, `-executors`, `-work-dir` (removed), `-max-output`; no batch for `--show-tests`. `runner-parallel-lit.test`: four lit runs on one build directory with 400 stale batch directories. `runner-cleanup-term.test`, `runner-cleanup-int.test`: SIGTERM and SIGINT leave no work directory.
- `runner-recycle.test` (the allowlist rule), `runner-fds.test` (descriptors above 900). Time limits: only a program that must hang has one (`// TIMEOUT: MS` in the program, sent in the manifest); a program that must exit keeps the 60 s default, so a loaded machine cannot fail it.
- `tests/run/runner-equivalence.long.test`: the whole run corpus through the fast runner and as real binaries, equal byte for byte (an address masked), both times printed; and `check-scaling.py`, which checks that the CPU cost per program stays the same at 1x, 4x and 16x the corpus (best of two, sizes in turn), after a control that must fail: a child whose work is quadratic in its size, measured the same way.
- `ninja check-run-real`: every run test again with real binaries.

## Numbers

Today's corpus is 107 programs (`compare-runner.py --list`: every program of `tests/run`, `Inputs/panics`, `Inputs/diff`, the init-order packages and the runner's own programs that end by themselves). Measured on the dev machine (192 cores) at load 4, 16 at a time for the real binaries:

| Way | Wall | CPU |
|---|---|---|
| 107 programs, one `gopp test-run` | 0.21 s | 1.1 s |
| 107 programs, real binaries (`gopp build` + exec) | 0.34 s | 3.3 s |
| 428 programs (4x), one `gopp test-run` | 0.42 s | 3.9 s |
| 1712 programs (16x), one `gopp test-run` | 1.3 s | 16 s |

In lit (`build/tests/run`, `lit -j 16`, load 10, two runs each): the 32 run tests that were there before the runner sum 2.2 s of test time plus 0.4 s of batch CPU, against 3.1 s with `-Dreal_binaries=1`; the floor of a test is now Python and lit, not the compiler. The runner's own 11 tests add 4.8 s (none over 0.8 s): they build real binaries to compare with, and two run a nested lit.

How it scales: about 9 to 10 ms of CPU per program, nearly all of it the compile (front end, LLVM at `-O0`, the object); a JIT link and run is under 1 ms. The cost per program from 4x to 16x is 1.06 times the cost from 1x to 4x (linear; the quadratic control gives about 4). So 100 programs take about 1 s of CPU, about 0.2 s of wall on many cores; the plan's M1 corpus budget (30 s, P04.14) leaves a factor of more than 100 for programs that are bigger than today's.

## Not done here

- Another target: only the host runs in the JIT. arm64 under QEMU stays real binaries (`--qemu`).
- The cached runtime and std (P05.01, P06): the request has no field for extra objects yet. Adding `"link":[paths]` to the run request and the job is an additive change.
- `gopp test` (a package with `_test.gopp` files, one test per request): the executor runs `main` only.
