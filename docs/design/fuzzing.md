# Fuzzing

Hand-written tests check the cases someone thought of. The fuzzer checks the rest: it makes programs nobody would write, runs every stage of `gopp` on them, and compares the results against things that must always be true. This document is the contract for `crates/gopp-fuzz` and `tests/fuzz/` ([ADR-0001](../adr/0001-implementation-language.md), [gir.md](gir.md), [pipelines.md](pipelines.md), [test-runner.md](test-runner.md)).

## 1. What it must find

| Bug class | Example | Found by |
|---|---|---|
| Accepts invalid code | a use after move, a dangling `*T` or a data race compiles | violation injection (section 5) |
| Miscompile | wrong output, wrong drop order, a value freed twice or never | differential and sanitizer oracles (section 4) |
| Rejects valid code | a legal program gets a compile error | valid-by-construction programs (section 3) |
| Crash | a panic in `gopp`, `E0440`, a verifier failure, emitted C that Clang refuses | every target |
| Hang or blow-up | quadratic checking, unbounded instantiation, a runaway `@Const` | step and memory bounds (section 7) |
| Nondeterminism | output depends on `-j`, the directory, the cache or thread order | determinism oracles (section 4) |
| Run-time library bug | a map loses a key, a channel drops a value, the scheduler deadlocks | model and schedule fuzzing (section 6) |

The first two classes are the reason the fuzzer exists. A compiler whose safety checks can be bypassed, or whose output is wrong, breaks the promise of the language while every hand-written test still passes.

## 2. Pieces

| Piece | Crate / place | Job |
|---|---|---|
| Targets | `tests/fuzz/` | one function per target: bytes in, verdict out |
| Mutators | `gopp-fuzz::mutate` | byte, token and syntax-tree mutation of existing programs |
| Generator | `gopp-fuzz::gen` | builds valid programs from a seed, with a model of ownership |
| Injectors | `gopp-fuzz::inject` | turn a valid program into an invalid one with a known expected error |
| Oracles | `gopp-fuzz::oracle` | decide whether a result is a bug |
| Reducer | `gopp-fuzz::reduce` | shrinks a failing program while it still fails the same way |
| Triage | `gopp-fuzz::triage` | groups findings, stores them, turns them into regression tests |
| Campaign driver | `gopp-fuzz` binary | runs targets under budgets, on one machine or many |

Rules that keep the pieces apart:

- **A target is a plain function over bytes.** `fn(&[u8]) -> Verdict` with no global state. Any engine can drive it: a coverage-guided engine, `proptest`, or a loop over seeds. No target depends on one engine.
- **A program is a seed.** The generator turns `(seed, config)` into the same program on every machine (`rand_chacha`). A finding is reproduced from the seed, the config and the compiler revision; the program text is stored too, so a generator change never loses a finding.
- **The generator does not call the checker.** It has its own small model of the rules (section 3). When the model and the checker disagree, one of them is wrong, and both are worth looking at.
- **`gopp-fuzz` is a development tool.** It is not part of the `gopp` binary. Fuzzing of user programs (`gopp test -fuzz`) is a separate feature.

## 3. The program generator

The generator writes Go++ programs that are valid, terminate, have no undefined behavior, and print everything they compute.

- **Typed from the start.** It picks a type first and then builds an expression of that type, so every program type-checks by construction. Types come from a per-program pool: scalars, strings, slices, maps, arrays, structs, `Option[T]`, pointer kinds, interfaces, func types, generic instances, `@Extends` hierarchies.
- **An ownership model.** For each variable the generator tracks its category (`OWN-C1`, `OWN-C2`, `OWN-C3`), whether it is live, moved or borrowed, which borrows of it are live, and whether it is a read-only binding. It emits an operation only when the model allows it: no read after a move, no `append` while `&xs[i]` is live, no capture of a writable local by a plain `go`. The model is written from the spec's rule text, rule by rule, and each generator choice names the rule that allows it.
- **Observable behavior.** Every program keeps a running checksum and prints it, with the values it computes, at fixed points. Types with `drop` log their identity when dropped, so drop order and drop count are part of the output. Panics are caught at the top and printed.
- **Bounded.** Loops have constant bounds, recursion has a depth parameter, allocation sizes are small. A generated program finishes in milliseconds.
- **No outside world.** No files, network, clock, environment or randomness. Concurrent programs make their output deterministic by construction (results are collected and sorted, or protected by one lock).
- **Feature knobs.** A config sets the weight of each feature: generics, generic methods, closures and captures, interfaces and type switches, `!` functions and `call!()`, `defer`/`panic`/`recover`, range forms including `&`, `@Operator` methods, `@Extends` with `@Virtual`/`@Override`, composite `const` and `@Const`, arenas, `Mutex[T]`, channels and `sync.Scope`. Campaigns rotate configs so no feature starves.
- **A Go twin.** In "Go subset" mode the generator uses only what Go and Go++ agree on and writes the same program as `main.go` and `main.gopp`. Real Go is then the reference.
- **Size classes.** Small programs (tens of lines) for speed and easy reduction; large programs (thousands of lines, many packages) for the build system, export data and the cache.

## 4. Oracles for valid programs

A valid generated program is run through the oracles below. Any "no" is a finding.

| Oracle | Question |
|---|---|
| Accept | Does `gopp check` accept it with no diagnostic? |
| Stage health | Did every stage finish with no panic, no `E0439`/`E0440`, `verify_module` clean after every pass, and emitted C that compiles with `-Wall -Werror` under the mandatory flags? |
| Go differential | In Go subset mode, is stdout, stderr and the exit code the same as Go's? |
| Opt matrix | Is the output identical at `-opt=0`, `1`, `2`, `3`, `s` and `z`? |
| Targets | Is the output identical on `linux/amd64` and on `linux/arm64` under `qemu-aarch64`? |
| Backend modes | Is the output identical when the package is emitted as `.c` and as `.cpp`? Through the fast runner and as a real binary? With `-panic=abort` where no `recover` is used? |
| Interpreter | For a `@Const` function, does compile-time evaluation give the same value as the compiled function? |
| Sanitizers | Does the program run clean under ASan + LSan, UBSan and TSan? |
| Accounting | In a counting build of the runtime, do allocations equal frees and retains equal releases at exit, and did every `drop` type log exactly one drop per value? |
| Metamorphic | After a change that must not matter, is the program still accepted with the same output? |
| Determinism | Are the diagnostics, the GIR text, the C, the `.gopx`, the object and the executable byte-identical across `-j1` and `-jN`, two directories, a cold and a warm cache, and two runs? |
| Round trips | `fmt(fmt(p)) == fmt(p)`; parse of the printed tree gives the same tree; `.gopx` decode of encode is the same module; `demangle(mangle(x))` is `x`. |

Changes "that must not matter" (the metamorphic set): reformatting; renaming locals; wrapping statements in a block; adding dead code after a `return`; reordering independent declarations; replacing a function with its generic form and an explicit instantiation; inserting `x.Clone()` where a value is lent; splitting a package in two; moving a helper to another package. The plain-versus-generic change is the most valuable: every rule must give the same answer in generic code as in plain code.

The sanitizer and accounting oracles are how ownership bugs show up. A wrong retain, a missed drop on an unwind path or a wrong last-use decision does not change stdout; it leaks, frees twice or reads freed memory.

## 5. Oracles for invalid programs

An **injector** takes a valid generated program and makes exactly one illegal change at a point the model says is legal to break. It knows which rule it broke and which error must come out.

| Injector (examples) | Rule | Must be rejected with |
|---|---|---|
| read a moved `OWN-C3` local | OWN-R5 | `E0451` |
| move out of a field or element | OWN-R3a | `E0253` |
| `append` to `xs` while `&xs[i]` is live | OWN-B2 | the borrow error |
| return `&local` or a stack-array slice | OWN-B3 | the borrow error |
| pass two overlapping writable pointers | OWN-B4, OWN-B9 | the borrow error |
| capture a writable local in `go func(){…}()` and use it after | CON-13 | the concurrency error |
| send a non-`Send` value on a channel | CON-14 | the concurrency error |
| leave a `@Required` field out of a literal | TYP-15 | the required-field error |
| `Option[T]` over a nilable `T` | OWN-P9 | `E0446` |
| read `v` of `v, err := f()` before checking `err`, for a no-zero-value `v` | OWN-P10 | the flow-typing error |
| insert into a map inside its own `range` | RNG-3 | the range error |
| override a final method, drop an `@Override` | TYP-18 | `E0384`, `E0382` |

Outcomes:

- **Rejected with the expected code**, at a span inside the injected text: pass.
- **Accepted**: the most serious finding the fuzzer can make. Before it is filed, the injected program is compiled and run under the sanitizers; a sanitizer report confirms that the model is right and the checker is wrong.
- **Rejected with another code**: a diagnostics finding (wrong or cascading error).

Each injector is also applied to the generic form of the program and across a package boundary, because the per-instance checks and export data are where rules are most easily lost. There is one injector for every rule that has a compile error; the list is checked against the spec's rule IDs, so a rule with no injector is visible.

## 6. Fuzzing below and beside the front end

- **Bytes, tokens and trees.** Mutated sources (bit flips, token deletion and duplication, splicing subtrees between corpus files, truncation at every byte) go through the lexer, parser, formatter and checker. The oracle is only: no panic, no hang, bounded memory, well-formed diagnostics (every span inside its file, every code in `codes.lock`, sorted, and every fix-it applies cleanly). Seeds are the spec samples, the test suites and the generator's output.
- **GIR.** A second generator builds random GIR modules directly with `FuncBuilder`, so drop elaboration, inlining, the FMA matcher, plugins and the C emitter are fuzzed without the front end. Pass-equivalence is the oracle: the module before and after a pass computes the same result in the interpreter. Mutating a valid module tests the verifier from the other side: a mutated module that passes `verify_module` must still compile and run without a crash; if it does not, the verifier has a missing rule.
- **Containers.** `.gopx` files, cache entries and `-fpass-plugin` wire payloads are truncated and bit-flipped; the result must be a clean error.
- **Symbols.** Random and mutated names through `gopp_mangle` and `gopp demangle`; long and deeply nested names must stay near-linear.
- **Run-time library.** Random operation sequences on maps, slices, strings and channels are run against a simple reference model under the sanitizers. This uses the library's C interface directly, with no compiler in the loop.
- **Schedules.** The scheduler has a seeded mode in which every yield point asks a random source which goroutine runs next. Channel, `select`, `Mutex[T]`, `WaitGroup` and `sync.Scope` tests are run under thousands of schedules; invariants (no lost value, no double delivery, every `Scope` joins, a deadlock is reported as a deadlock) are checked on each. A failing schedule is replayed from its seed.
- **Foreign headers.** Generated C headers (structs, unions, bit-fields, function pointers, macros) are imported; layouts must equal Clang's, and values passed through generated C functions must come back unchanged.
- **Macros.** Generated declarative macros and call sites check hygiene (no capture in either direction) and that expansion is deterministic.

## 7. Bounds

Every run of everything is bounded and seeded:

- Wall time per input (`timeout`), CPU time, memory (`RLIMIT_AS`), output size (`RLIMIT_FSIZE`), and a step budget in every analysis. Hitting a step budget on an input of size `n` with more than `c · n` steps is a finding of its own.
- Generated programs run inside `gopp test-exec` workers ([test-runner.md](test-runner.md)), which already isolate crashes, timeouts and runaway output.
- A campaign has a total time box and a disk budget, and writes only under its own work directory.
- Loops that grow an input ("make it bigger until it is slow") have hard caps on size and rounds.

## 8. From finding to fix

1. **Reduce.** The reducer shrinks the program by removing declarations, statements, fields and arguments and by simplifying expressions, re-running only the oracle that failed. It works on the syntax tree, so every candidate still parses; for valid-program findings it re-checks each candidate with the model so the reduced program is still valid.
2. **Group.** A finding's key is the oracle, the stage, and a signature: the panic location, the diagnostic code, the verifier rule, the sanitizer report kind, or the pair of configurations that differ. New findings with a known key are counted, not filed again.
3. **Store.** The reduced program, the seed, the config, the revision and the oracle output go to `tests/fuzz/findings/`.
4. **Fix with a test.** The reduced program becomes a normal test in the suite of the crate that was wrong. The test must fail before the fix. A finding is closed only then.

Severity order: accepts invalid code, then miscompile, then crash, then rejects valid code, then nondeterminism, then hang or blow-up.

## 9. Knowing what was fuzzed

- **Code coverage** of the compiler crates per campaign.
- **Feature coverage:** which spec rule IDs the generator exercised, which diagnostic codes were produced, which verifier rules fired, which GIR statement, rvalue and terminator kinds and which runtime entry points were reached. The lists come from the spec, `codes.lock` and the GIR definitions, so a rule, code or node kind that no input ever reached is printed by name.
- Campaigns raise the weight of features with low coverage.

## 10. Where it runs

| Run | Size | Gate |
|---|---|---|
| Smoke, in `cargo test --workspace` | fixed seeds, a few hundred small programs and a few thousand mutated inputs, inside the normal time budget | a finding fails the suite |
| Nightly | hours, all oracles, both targets, the opt matrix | findings are filed |
| Continuous | a dedicated machine, coverage-guided engines on the byte-level targets, the generator on the rest | findings are filed; corpus is kept |

The smoke run counts steps and compares results; it never measures wall time.

## 11. Open points

- **Engine.** The coverage-guided engine for the byte-level targets must run on the pinned stable toolchain. Candidates are AFL++, honggfuzz and LibAFL; libFuzzer through `cargo-fuzz` needs a nightly compiler. The choice is made by trying them in the Nix shell and is recorded in an ADR with the crate versions.
- **Go as a reference** needs the pinned Go in the dev shell.
