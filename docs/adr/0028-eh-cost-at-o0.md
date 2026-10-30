# ADR-0028: Exception handling in debug builds: keep FastISel, give each invoke its own block

- **Status:** accepted (decisions 1 to 5, 7 and 8, from the P04.15 measurements; they change nothing that users see). Decision 6 is **proposed**: it needs the owner, and only if P06.01 triggers it.
- **Date:** 2026-10-30
- **Plan reference:** P04.15 (the EH cost spike); P04 D4.6 (panics are C++ exceptions), D4.2 step 3 (drops on unwind paths), D4.8 (the `invoke` risk); P06 D6.6 (pipelines), D6.8 (budgets), D6.9 (the baseline-backend fallback); P00 D0.13 (dev-loop budgets); P01 D1.4 rule 2 and D1.9 (drop and `defer` order), P07 D7.8 (`@Unwind`), spec UB-4, ADR-0013 (stack overflow panics), ADR-0023 (drop order of a child); the README risk "Panic cleanup turns most calls into `invoke`".

## Context

Panics are native C++ exceptions (D4.6). A call that may unwind while the caller still has something to drop (an owning local, an owning temporary, a `defer`) becomes an LLVM `invoke` with a cleanup landing pad. Bounds, division, assertion and nil checks branch to a panic call, which unwinds too, so it is also an `invoke` in that case.

At `-O0`, x86-64 LLVM uses FastISel. FastISel cannot select `invoke` or `landingpad`, so it hands them to SelectionDAG, which is about 2.5 times slower. The plan feared this would make debug builds slow (risk R4) and offered three ways out: patch FastISel, use GlobalISel, or build our own debug backend (D6.9, size XL). This spike measures the cost before P04.07 builds the panic lowering.

## Setup

**Machine and tools.**
- AMD Threadripper PRO 9995WX (znver5), 192 cores, shared with other jobs.
- LLVM 23.1.0 from the Nix flake: a release build, without assertions.
- The release build is enough. `-pass-remarks-missed=sdagisel` reports one remark per instruction that FastISel hands to SelectionDAG. So `-stats` and the assertions shell are not needed.
- Command: `llc <selector flags> -filetype=obj -o /dev/null` on bitcode, each run under `timeout 60`, pinned to one core (`taskset -c 191`).
- The input is bitcode (`llvm-as`), not `.ll` text. Parsing the text takes about half of `llc`'s time and hides the codegen difference. `gopp` hands LLVM its IR in memory.

**What is measured.**
- **CPU time** (user + system) of the `llc` process: best of 3 runs. The runs go round-robin over all configurations, so a burst of load hits all of them alike.
- **Codegen passes** and **ISel passes:** CPU time from `-time-passes`.
- **Instructions:** user-space instructions retired by one run (`perf stat -e instructions:u`). This number does not change with machine load.

The machine's load average swung between 0.3 and 420 during the work.
- Time sessions whose runs differed by more than a few percent were thrown away.
- Table 1 is the exception: it is the best of three sessions (9 runs). Each session alone gave the same ratios within ±0.1.
- Instructions are the cross-check, and they agree on every conclusion. They grow less than time, because the SelectionDAG work also runs at a lower IPC. So time stays the main number.

**Proportions: measured on Go code.** Go++ code has Go's shape, and there is no Go++ corpus yet.
- `bench/eh/goshape.go` type-checks 22 standard-library packages: 3663 functions (with func literals), 64983 body lines.
- For every function it counts what decides the EH code.
- A call or check "needs a pad" when the callee may unwind and something must be dropped at that point:
- an owning local in scope: `string`, slice, map, chan, interface, func, a pointer made by `&T{}` or `new`, or a struct or array holding one. Parameters and range values are borrowed in Go++, so they don't count;
- an owning temporary of an enclosing call;
- a `defer`.

| Per 100 Go lines | Measured |
|---|---|
| calls that may unwind | 17.1 (89% of calls; the rest call bodies that cannot panic) |
| ... of which need a pad | 10.3 (60% of unwinding calls) |
| bounds checks | 5.7 |
| assertion / division / nil checks | 0.4 / 0.04 / 0.03 |
| ... checks that need a pad | 3.3 (54%) |
| owning locals | 4.6 |
| small struct arguments / multi or struct results (of which `(T, error)`) | 0.9 / 3.3 (1.8) |
| `if` / `for` statements | 11.2 / 1.9 |
| functions with `defer` | 7.2% of functions |
| owners live at a call that needs a pad | 2.56 on average |
| body lines per function, p10/p25/p50/p75/p90/p99 | 3/4/9/19/41/137 |

**The generator.** `bench/eh/gen-eh-ir.py` draws one program from these rates and writes it as `-O0` IR, in the same style as Clang's `-O0` output:
- every local lives in an `alloca`;
- owners are zeroed at entry, dropped at the end of their block and on every exit, and dropped on unwind (the drop glue skips all-zero values, D4.2);
- landing pads follow Clang's layout: one pad per cleanup state, and chained cleanup blocks that drop the innermost owner first;
- checks branch to cold, `noreturn` panic calls;
- functions with `defer` get a catch-all pad that runs the deferred call, asks the runtime about `recover`, and rethrows (D4.6).

The default size is 9000 source lines and 417 functions, which gives **101,002 IR lines**. The generated program matches the measured one:

| Shape | Go (measured) | Generated (seeds 1 / 2 / 3) |
|---|---|---|
| unwinding calls / bounds checks per 100 lines | 17.1 / 5.7 | 17.5 / 5.6 (seed 1) |
| small struct arguments / multi or struct results per 100 lines | 0.9 / 3.3 | 1.0 / 3.3 (seed 1) |
| unwinding sites (calls and checks) that are `invoke`s | 58.5% | 56% / 57% / 61% |
| owners live at an `invoke` | 2.56 | 2.17 / 1.89 / 2.08 |

The same program is written in several ways, so every row below compiles the same code:

| Name | What it is |
|---|---|
| `eh` | D4.6 as planned: `invoke`s, landing pads, a personality |
| `noeh` | the same calls as plain calls, no pads, every function `nounwind`: what `-panic=abort` emits; `eh` minus `noeh` is the cost of EH |
| `shape` | `eh`'s CFG and cleanup code, but with plain instructions in place of the EH ones (a call and a two-way branch instead of `invoke`, plain loads instead of `landingpad`, a call instead of `resume`). This is what a FastISel that handled EH would see, so it is **the best a FastISel patch can do** |
| `eh+S` | `eh` after `bench/eh/split-invokes.py`, which moves each `invoke` into a block of its own |
| `eh+MS` | `eh` with one landing pad per function that drops every owner (`--pads single`) and shared panic calls (`--panics shared`), then split. **Not the exact drop order** (see decision 3.2); kept to show what the order costs |
| `eh+TS` | decision 3 as adopted: one landing pad per function with a cleanup-state index (`--pads state`: each site stores its index, the pad dispatches with compares into the per-scope cleanup chains, so the order is exact), shared panic calls, then split. At `opt 0` also LLVM's `lower-switch`, because simplifycfg turns the compares back into a `switch` |
| `split` / `agg` ABI | `split` (the default in the tables): small structs pass as scalars, aggregate results return through a hidden pointer. `agg`: first-class `{i64, i64}` arguments and `{i64, {ptr, ptr}}` returns (D4.4's "in registers") |

**Two pipelines.**
- **Plain `-O0`:** the generator output goes straight to `llc -O0`. This is the `-pipeline=fastisel` baseline of D6.6.
- **`opt 0`:** first `opt -passes='sroa,instcombine<no-verify-fixpoint>,simplifycfg'`, then `llc -O0`. This approximates D6.6's curated pass set; SROA covers mem2reg, and there is nothing to always-inline. The IR shrinks to 38k lines. simplifycfg merges compatible `invoke`s, so 1151 of 1246 remain. `eh+S` here is split after the passes, as a codegen-time pass would do it.

**Selectors.**
- `FastISel`: the default at `-O0` on x86-64.
- `SDAG`: `-fast-isel=false`.
- `GlobalISel`: `-global-isel -global-isel-abort=2`. When GlobalISel cannot handle a function, it falls back to SelectionDAG for that whole function and prints a warning, which is counted.
- On aarch64, GlobalISel is LLVM's default at `-O0`, and `-global-isel=false` gives FastISel.

## Results

CPU seconds of `llc` for 101k IR lines (9000 Go++ lines). "vs `noeh`" compares with FastISel on `noeh` code.

**Table 1: x86-64, plain `-O0`.**

| IR | FastISel | vs `noeh` | codegen passes / ISel | SDAG | GlobalISel | instructions (G), FastISel | FastISel fallbacks |
|---|---|---|---|---|---|---|---|
| `noeh` | **0.121** | — | 0.067 / 0.024 | 0.322 | 0.182 | 1.43 | 21 (argument lowering) |
| `shape` | 0.130 | +7% | 0.072 / 0.026 | 0.363 | 0.189 | 1.53 | 21 |
| `eh` | 0.202 | **+67%** | 0.138 / 0.082 | 0.351 | 0.193 | 2.05 | 1770: 1238 `invoke`, 511 `landingpad`, 21 args |
| `eh+S` | 0.162 | +34% | 0.102 / 0.048 | 0.364 | 0.197 | 1.76 | 1789 |
| `eh+MS` (inexact) | 0.159 | +31% | 0.099 / 0.045 | 0.356 | 0.195 | 1.71 | 1330: 1066 `invoke`, 243 `landingpad`, 21 args |

**Table 2: x86-64, `opt 0` pass set, then `llc -O0`** (two sessions, equal within 2 ms).

| IR | FastISel | vs `noeh` | codegen passes / ISel | GlobalISel | instructions (G), FastISel / GlobalISel | FastISel fallbacks |
|---|---|---|---|---|---|---|
| `noeh` | **0.078** | — | 0.048 / 0.014 | 0.096 | 0.75 / 1.02 | 57 |
| `shape` | 0.080 | +3% | 0.049 / 0.015 | 0.100 | 0.78 / 1.07 | 57 |
| `eh` | 0.142 | **+82%** | 0.106 / 0.060 | 0.110 | 1.21 / 1.14 | 1757: 1151 `invoke`, 411 `br`, 141 `landingpad`, ... |
| `eh+S` | 0.114 | +46% | 0.081 / 0.037 | 0.111 | 1.06 / 1.17 | 1779 |
| `eh+MS` (inexact) | 0.107 | +37% | 0.073 / 0.032 | 0.108 | 0.97 / 1.13 | 1281: 994 `invoke`, 204 `landingpad`, 24 `br`, ... |

Tables 1 and 2 predate decision 3.2. The adopted exact-order pad (`eh+TS`) is in Table 6: +30% / +42%.

The `br` fallbacks come from landing pads. After SROA, a pad passes its `{ptr, i32}` exception value to a shared cleanup block through a PHI, and FastISel cannot lower that PHI. With one pad per function there is nothing to share, so they almost disappear (411 down to 24).

**Table 3: x86-64, aggregate ABI (`agg`).**

| IR | FastISel plain / `opt 0` | GlobalISel plain / `opt 0` | GlobalISel functions that fall back | FastISel fallbacks from aggregates |
|---|---|---|---|---|
| `noeh` | 0.158 / 0.109 | 0.440 / 0.206 | 199 / 194 of 417 | 251 calls, `insertvalue` and `ret` (plain) |
| `eh` | 0.235 / 0.174 | 0.402 / 0.235 | 199 / 194 of 417 | 153, besides the EH ones |

Compared with the `split` ABI, the aggregates cost FastISel +5 to 8% in instructions on `noeh`, and +21 to 40% in time in single sessions that could not be repeated cleanly. P04.18 re-measures this on real IR.

**Table 4: aarch64, `split` ABI** (one clean session).

| IR | GlobalISel (LLVM default) plain / `opt 0` | FastISel plain / `opt 0` |
|---|---|---|
| `noeh` | 0.187 / 0.101 | 0.125 / 0.072 |
| `eh` | 0.207 / 0.119 | 0.193 / 0.131 |
| `eh+MS` | 0.206 / 0.114 | 0.153 / 0.101 |

**Table 5: other generated programs** (x86-64, FastISel, instructions in G, plain / `opt 0`; the time sessions for seeds 2 and 3 were spoilt by load).

| Seed | `noeh` | `eh` | `eh+MS` | EH cost, `eh` | EH cost, `eh+MS` |
|---|---|---|---|---|---|
| 1 | 1.43 / 0.75 | 2.05 / 1.21 | 1.71 / 0.97 | +44% / +62% | +20% / +29% |
| 2 | 1.47 / 0.78 | 2.14 / 1.28 | 1.77 / 1.02 | +45% / +64% | +20% / +30% |
| 3 | 1.49 / 0.78 | 2.17 / 1.28 | 1.79 / 1.02 | +46% / +64% | +21% / +31% |

An earlier quiet session on `.ll` inputs with a previous generator version gave the same picture in time for all three seeds: `eh` +54 to 59% / +74 to 75%, `eh+MS` +26 to 29% / +34 to 35%. Those ratios are lower because they include the text parse.

**Table 6: the exact-order single pad (decision 3.2).** A later quiet session (load 0.3), x86-64, FastISel, CPU seconds, plain / `opt 0`; percentages against this session's `noeh`.

| IR | CPU | vs `noeh` | instructions (G) | FastISel fallbacks |
|---|---|---|---|---|
| `noeh` | 0.122 / 0.078 | — | 1.43 / 0.75 | 21 / 57 |
| `eh` | 0.199 / 0.140 | +63% / +79% | 2.05 / 1.21 | 1770 / 1757 |
| `eh+MS` (inexact) | 0.156 / 0.106 | +28% / +36% | 1.71 / 0.97 | 1330 / 1281 |
| `eh+TS` with a `switch` dispatch (at `opt 0`: compares that simplifycfg turned back into a `switch`) | 0.167 / 0.113 | +37% / +45% | 1.75 / 1.01 | as below + 182 / 34 `switch` |
| **`eh+TS`** (compares; `lower-switch` at `opt 0`) | **0.159 / 0.111** | **+30% / +42%** | 1.74 / 1.01 | 1330 / 1281 |
| `shape` of `eh+TS` (bound of a patch) | 0.133 / 0.081 | +9% / +4% | 1.55 / 0.78 | 21 / 59 |

- The exact order costs 2 points at plain `-O0` and 6 points at `opt 0` over the inexact single pad: one store of the state index per site, and the dispatch.
- The dispatch must not be a `switch`: FastISel cannot select one, and x86-64 GlobalISel falls back for the whole function on the jump table (`G_BRJT`, 31 functions).

**What the numbers say.**
1. **EH makes x86-64 FastISel codegen 1.7 to 1.8 times slower** on Go++-shaped code: +67% with plain `-O0`, +82% with the `opt 0` passes. The codegen passes alone take 2.1 to 2.2 times as long.
2. **Almost all of this cost is the fallback, not the cleanup code.** `shape` has the same CFG and the same drops as `eh`, and costs only +7% / +3% over `noeh`.
3. **One failed `invoke` hands its whole block to SelectionDAG.** FastISel selects a block bottom-up, so when the terminator fails, every instruction above it (the argument loads and GEPs) goes to SelectionDAG too.
   - With the `invoke` alone in its block, SelectionDAG sees one instruction. That removes about half of the EH cost (`eh+S`).
   - An `invoke` fallback costs about 50 µs inside its block and about 20 µs alone. A `landingpad` fallback costs about 15 µs.
4. **The lowering changes of decision 3 reach +30% / +42%** with the exact drop order (`eh+TS`, Table 6). The inexact single pad would reach +28% / +36%. All they need is IR shape: no change to LLVM, and none to what users see.
   - **All EH cost figures in Tables 1 to 6 are lower bounds.** The generator calls 2.3 bodies per 100 lines that "cannot unwind", against 17.5 unwinding calls; with the prologue rule of decision 3.4 most of those become unwinding calls too, so up to about 13% more of them.
   - The generator's `defer` functions (23 of 417) keep their catch-all pad and do not store a state index, which adds well under 1 point.
   - The EH cost grows about in step with the number of `invoke`s. A rough upper estimate is therefore +35% / +48% with decision 3 (and about +93% at `opt 0` without it).
5. **A perfect FastISel patch would save 16% / 27% of `llc` time** on top of decision 3 (`eh+TS` down to its `shape`). Without decision 3 it would save 36% / 44%.
6. **GlobalISel on x86-64 does not help.**
   - On EH code it equals the mitigated FastISel with `opt 0` (0.110 against 0.107) and is 21% slower with plain `-O0` (0.193 against 0.159).
   - On code without EH it is 50% / 23% slower than FastISel.
   - It falls back for whole functions on first-class aggregate calls and returns: 199 of 417 functions with the `agg` ABI, about twice the time.
   - A coverage probe (`cmpxchg`, `frem`, `llvm.sadd.with.overflow`) also falls back.
   - LLVM does not use it on x86-64 at any level.
7. **SelectionDAG at `-O0` is the slowest everywhere** (0.32 to 0.36 s plain). It is not an option.
8. **On aarch64, FastISel with the decision-3 lowering beats LLVM's default GlobalISel**: 15% faster on EH code with `opt 0` (0.101 against 0.119; measured with the inexact single pad, and the exact one costs 2 to 6 points more on x86-64), 29% faster on code without EH. GlobalISel's cost is flat: EH adds only +11 to 18% to it.
9. **First-class aggregates in calls and returns (D4.4) also make FastISel fall back**, on x86-64 and aarch64 (Table 3). This is not about EH, but it matters to P04.18.
10. **Absolute size.** Per 100k Go++ lines (11.1 times the corpus), `opt 0`, one core:
    - without EH: 0.87 s;
    - planned EH: 1.58 s;
    - EH with decision 3: 1.24 s;
    - the bound of a perfect patch: 0.90 s.

    With D6.5's 8 partitions that is 0.11 / 0.20 / 0.15 / 0.11 s of wall time.

## Decision

1. **D4.6 stays as it is.** Panics are native C++ exceptions with cleanup landing pads. The cost is a constant factor that the lowering can halve, not a reason to change the panic design.
2. **x86-64 debug codegen (`opt 0` and `-pipeline=fastisel`) keeps FastISel**, LLVM's default. It does not use GlobalISel.
3. **P04.07 lowers EH for FastISel:**
   1. *Each `invoke` alone in its block:* a small IR pass runs right before instruction selection, after the `opt 0` passes (simplifycfg would merge the blocks back). It moves every `invoke` that is not the only instruction of its block into a new block, and renames the successors' PHI entries (`bench/eh/split-invokes.py` is the reference). It runs in the pipelines that use FastISel (decision 2; on arm64, decision 7).
   2. *One landing pad per function, driven by a cleanup-state index.* A pad that just runs "the defers, then drop everything" would change the order users can observe, so the pad is state-driven:
      - Each unwinding site stores a constant **cleanup-state index** into a slot before its `invoke` (or before the branch of a check).
      - The index names one point in the function's creation sequence. Its cleanup is exactly what per-scope pads would run there (P01 D1.4 rule 2, D1.9):
        1. the live block-scoped locals and owning temporaries, innermost first, each scope in reverse creation order;
        2. at the function scope: the deferred calls, LIFO, in the catch handler of D4.6 (`recover`, then rethrow);
        3. then the function-scope locals and the `defer`-extended locals, in reverse creation order.
      - Inside a value, ADR-0023's order holds: the child's `drop`, its fields in reverse, then the parent part.
      - The pad dispatches on the index with compares and branches into chained cleanup blocks, as Clang's per-scope pads are chained. It never uses a `switch`: FastISel cannot select one (Table 6). At `opt 0` the codegen-time pass of 3.1 also runs LLVM's `lower-switch`, because simplifycfg rebuilds a `switch` from the compares.
      - Owning temporaries of an enclosing call have their own slots, part of the state, in reverse creation order.
      - A `move` argument's slot is zeroed before the `invoke`: the callee owns it and drops it on every path, panics included (D1.9).
      - A value moved out of a local zeroes the slot (D4.2), and drop glue skips zero values, so a moved local needs no new state. C++-typed locals are covered by the index too; they have no all-zero "dead" state, but the index says exactly which ones were constructed.
      - Example: in `func f() { a := S("a"); { b := S("b"); defer print("d"); g() } }`, a panic in `g` runs `drop b`, then `d`, then `drop a`.
   3. *One panic call per function and panic kind.* Each check stores its operands, its cleanup-state index (3.2) and its **source position** (a constant), and branches to that call.
      - The runtime reports the stored position, so every check keeps its own line in the panic message and the stack trace (D4.7; P04.14 compares stderr with Go).
      - "Cleanup state" in an earlier draft meant one panic call per state. With the index of 3.2 the check stores its state before the branch, so one call per kind is enough, for Go++ owners and C++ locals alike.
      - At `opt 0`, simplifycfg already merges such `invoke`s.
   4. *No pad for callees that cannot unwind,* with a sound definition. A wrong "cannot unwind" is undefined behavior (UB-4: an exception through a frame marked `nounwind` by mistake). So:
      - **Cannot unwind:** a Go++ body with no call that may unwind, no check (bounds, division, assertion, nil, the C-call stack headroom of ADR-0013), and no prologue stack check. ADR-0013 makes a stack overflow panic, and only small leaf functions in the red zone skip that check. If P12 makes the overflow a fatal error that does not unwind, this last condition goes away.
      - Computed by a fixpoint over the call graph: a recursive cycle cannot unwind only when no member can. With the prologue rule above, a recursive function always may unwind for now.
      - Carried as a `nounwind` bit per function in export data (P06 D6.2), in a section whose hash the callers rebuild on. A body that starts to unwind recompiles its callers, so a stale bit cannot cause UB-4.
      - **Everything else may unwind:**
        - indirect calls: func values, interface methods, `@Virtual` dispatch, `ffi.Func`;
        - C functions, because the build compiles C with `-fexceptions` (D7.8) and a C library may call an `@Unwind` callback it stored earlier. The only exception is a C function declared `nothrow` (glibc's `__THROW`, which Clang turns into `nounwind`) that takes no function pointer. A wrong `nothrow` in a header is that header's bug (UB-4);
        - C++ functions that are not `noexcept` (or `throw()`);
        - runtime helpers that can panic (map and channel operations, conversions, allocation of user types). A helper is `nounwind` only if the runtime declares it so.
      - The measured 11% of calls that "cannot panic" is an upper bound, because the measurement did not apply the prologue rule.
      - **Under `-panic=abort`:** every Go++ function is `nounwind`, and Go++ code has no cleanup pads. A call that may let a foreign exception in (C or C++ by the list above, `ffi.Func`) gets an abort-on-unwind pad (D4.6, D7.8). Calls to Go++ code (direct, func values, interface methods) get none.
4. **D6.9 is not triggered by EH.** With decision 3, EH adds 0.37 s of CPU per 100k Go++ lines at `opt 0` (0.05 s wall on 8 workers). This is a lower bound (numbers point 4); the rough upper estimate is 0.43 s CPU, still 0.05 s wall. A baseline backend (size XL) is far out of proportion. D6.9 stays a P06 fallback for other reasons only.
5. **No LLVM patch now.** A FastISel that selects `invoke` and `landingpad` would save at most 16% / 27% of `llc` time after decision 3.
6. **Proposed, owner decision, only if triggered.** If P06.01 measures that the D6.8 debug budget is missed, and the EH fallbacks are a large part of the gap, then:
   - patch generic FastISel to select `invoke` and `landingpad` (X86 and AArch64), and send the patch upstream;
   - until it is upstream, build LLVM from source with the patch, as the assertions shell does.

   This changes ADR-0002 ("LLVM comes from nixpkgs"), adds a patch to carry across LLVM upgrades, and makes every developer and the release build compile LLVM. It is not decided here.
7. **arm64:** P04.13 chooses the `opt 0` selector with real Go++ IR. Until then it uses LLVM's default (GlobalISel). The spike's numbers favour FastISel with decision 3 (−15% on EH code, −29% on code without EH). GlobalISel is LLVM's default and its best-tested AArch64 `-O0` path, so the switch needs P04.13's run tests first.
8. **P04.18 input:** at `opt 0`, first-class aggregate arguments and returns make FastISel fall back, and make x86-64 GlobalISel give up on whole functions (Table 3). P04.18 decides how `(T, error)` returns are lowered there, and re-measures this cost.

## Impact on the budgets

- **D6.8 (debug build of a 100k-line project ≤ 2× `go build`, calibrated in P06.01).** `llc` time at `opt 0` per 100k Go++ lines:
  - with the planned EH: +0.71 s CPU;
  - with decision 3: +0.37 s CPU, 0.05 s wall on D6.5's 8 partitions.

  `go build` itself was not measured here (P06.01 does it). This ADR does not show the budget out of reach. P06.01 re-measures the EH share on its generated project (decision 6).
- **D6.6 (`opt 0` ≤ 1.5× the plain `-O0` + FastISel compile time).** The EH factor hits both pipelines, so it barely moves this ratio. On the `eh` corpus the `opt 0` codegen is cheaper than plain `-O0` (0.142 against 0.202 s), but the IR passes themselves were not timed.
- **D0.13 (the dev loop of `gopp` itself).**
  - A codegen lit test compiles tens to hundreds of lines. EH adds about 1 ms per 100 lines at most, nothing against the 2 s limit per test.
  - The M1 corpus of P04.19 (≥ 100 programs, ≤ 30 s) gains well under 0.1 s.
  - The full suite (≤ 5 min) is not affected. The only new test, `tests/driver/eh-spike-tools.test`, takes 0.3 s.
  - No command in the spike took 60 s or more. The longest was one measuring run of 34 s; a full table takes 4 to 22 s.

## Consequences

- P04.07 can start: the panic design is fixed, and its lowering has four concrete rules (decision 3). P04.07's Done-when gets tests for the drop and `defer` order, the `nounwind` rule on both sides, per-check source lines, the IR verifier on the split pass, and the FastISel fallback count.
- Debug builds stay on stock LLVM from Nix. No fork, and no new dependency.
- The tools stay in `bench/eh/`, so P06.01 and P04.13 can re-run them on real Go++ IR. `perf` (for instruction counts) is optional and not in the flake: `measure-eh.py --perf PATH`.
- Not measured:
  - real Go++ IR (there is no codegen yet; the generator models it from Go code);
  - DWARF (`-g`, D4.7), which adds work but no fallbacks;
  - the in-process pipeline of `gopp`;
  - the time of the `opt 0` IR passes themselves;
  - whether the generated objects run correctly (`llc` only compiled them).
  - x86-64 GlobalISel was checked for fallbacks, not for code or debug-info quality.
  - `defer` functions in `--pads state`: the generator gives every function with `defer` (7.2%) its catch-all pad (`dpad`) without a state index or dispatch, so Table 6 understates the state-pad cost for them.
  - The prologue rule of decision 3.4: the generator treats the measured 11% of calls as calls that cannot unwind. With the rule, more of them are `invoke`s, so every EH cost figure here is a lower bound (numbers point 4).

## Alternatives

- **Patch FastISel now.** Rejected for now: the gain after decision 3 is 16 to 27% of `llc` time, and the cost is an LLVM fork until upstream accepts it (decision 6).
- **GlobalISel at `-O0` on x86-64.** Rejected: no faster on EH code than FastISel with decision 3, 23 to 50% slower on other code, whole-function fallbacks on aggregates, atomics and `frem`, and not LLVM's default anywhere on x86-64.
- **SelectionDAG at `-O0` (`-fast-isel=false`).** Rejected: the slowest option, 0.32 to 0.36 s.
- **The D6.9 baseline backend.** Rejected for EH: size XL, plus its own LSDA, CFI and DWARF, to win back at most 0.37 s of CPU per 100k lines.
- **One landing pad that runs "the defers, then drop everything".** Rejected: it changes the observable order (decision 3.2's example would run `d` before `drop b`). The exact state-driven pad costs 2 to 6 points more (Table 6).
- **`-panic=abort` as the debug default.** It would compile like `noeh`, but `recover` would not work and deferred calls would not run on a panic. That is a visible change in meaning, so it is not proposed. It stays an opt-in flag (D4.6).
- **setjmp/longjmp panics.** Already rejected in D4.6.

## Reproduce

```sh
nix develop -c go run bench/eh/goshape.go strings strconv bytes fmt bufio sort \
  encoding/json encoding/binary text/template go/parser go/scanner regexp \
  regexp/syntax net/url net/http compress/flate archive/tar image/png math/big \
  crypto/sha256 os path/filepath                       # proportions, about 3 s
python3 bench/eh/gen-eh-ir.py --abi split --mode eh > eh.ll      # also noeh, shape
python3 bench/eh/gen-eh-ir.py --abi split --mode eh --pads state --panics shared > ehT.ll
python3 bench/eh/split-invokes.py < ehT.ll > ehTS.ll    # at opt 0: opt -passes=lower-switch first
nix develop -c opt -passes='sroa,instcombine<no-verify-fixpoint>,simplifycfg' -S eh.ll -o o0-eh.ll
nix develop -c llvm-as eh.ll -o eh.bc                            # time bitcode, not text
nix develop -c taskset -c 191 python3 bench/eh/measure-eh.py eh.bc ehTS.bc noeh.bc shape.bc
#   --selectors fast,dag,gisel (x86-64) or a64-gisel,a64-fast (with --triple aarch64-unknown-linux-gnu)
#   --perf /path/to/perf adds instruction counts
```
