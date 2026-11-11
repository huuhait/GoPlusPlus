# Codegen pipelines

This document specifies what happens between a checked package (`gopp-sema`) and a compiled object file, how `-opt` and `-fpass-plugin` configure the stages, and how later profile/LTO features plug in ([ADR-0001](../adr/0001-implementation-language.md), [ADR-0002](../adr/0002-clang-version-policy.md), [ADR-0007](../adr/0007-build-profiles.md)). Implementation lives in `crates/gopp-gir` (GIR passes and verifier), `crates/gopp-plugin-host` (`-fpass-plugin` execution), and `crates/gopp-codegen-c` (C23 emitter and `ClangDriver`).

## 1. Core rule: identical program semantics in every pipeline

Optimization settings change compile time, binary size, and run-time speed — never what a valid program computes. Standard output, exit status, panic messages, and floating-point results are identical across `-opt=0`, `1`, `2`, `3`, `s`, and `z` on every target:

- **Zero C undefined behavior:** `gopp-codegen-c` compiles all generated GNU C23 with `-std=gnu23 -fwrapv -fno-strict-aliasing -ffp-contract=off -fno-builtin-malloc -fno-builtin-calloc -fno-builtin-realloc -fno-builtin-free`, and lowers Go shift semantics (`count >= width ? 0 : ...` / sign-fill), `MinInt / -1`, `MinInt % -1`, and saturating float-to-integer casts explicitly in C23 helpers.
- **Deterministic FMA contraction:** Floating-point multiply-add fusion (`__builtin_fma` / `__builtin_fmaf`) is decided once by `gopp-codegen-c` on `arm64` only, while `-ffp-contract=off` prevents Clang's optimizer from fusing additional operations at higher `-O` levels (section 4).
- **Allocation observability:** `-fno-builtin-malloc` (and matching `calloc`/`realloc`/`free` flags) prevents Clang from eliding failing oversize allocations (`make([]byte, 1<<48-1)`) above `-opt=0`.
- **Verified middle-IR plugins:** External `-fpass-plugin=<path>` passes run only after mandatory GIR safety checks and are followed immediately by `gopp_gir::verify_module` (`V001`–`V085`, [gir.md](gir.md) section 8.1).

## 2. Single definition point: `PipelineSpec`

`select_pipeline(PipelineRequest, &TargetSpec)` is the sole function translating CLI/profile settings into a `PipelineSpec`:

| Setting | Name | GIR trivial inlining | Middle-IR `-fpass-plugin` | Clang optimization flag | LTO (`-flto=thin`) |
|---|---|---|---|---|---|
| `-opt=0` (default) | `opt0` | yes | when passed on CLI | `-O0` | `none` |
| `-opt=1` | `opt1` | yes | when passed on CLI | `-O1` | `none` |
| `-opt=2` | `opt2` | yes | when passed on CLI | `-O2` | `none` or `thin` |
| `-opt=3` | `opt3` | yes | when passed on CLI | `-O3` | `none` or `thin` |
| `-opt=s` | `opts` | yes | when passed on CLI | `-Os` | `none` or `thin` |
| `-opt=z` | `optz` | yes | when passed on CLI | `-Oz` | `none` or `thin` |

### Stages in order

Each stage records wall and CPU timing in `PipelineTimes` (`-pipeline-times` prints `stage=wall/cpu`):

1. **`gir-lower` & `gir-verify`:** Lowers `gopp_sema::CheckedPackage` to `gopp_gir::Module` and runs `verify_module` (`V001`–`V085`). (`-emit=gir` prints the module here.)
2. **`gir-safety`:** Runs `MoveCheck`, `BorrowCheck`, and `ConcurrencyCheck` over the GIR CFG. Always runs unconditionally before any plugin or optimization pass.
3. **`plugin-pre-drop`:** Runs registered `PassStage::PreDropElab` plugins (`-fpass-plugin`), executing `verify_module` after each plugin.
4. **`drop-elab` & `gir-inline`:** Elaborates moves and drops (`elaborate_drops`), runs `inline_trivial` (section 3), and re-verifies with `verify_module`.
5. **`plugin-post-drop`:** Runs registered `PassStage::PostDropElab` plugins (`-fpass-plugin`), executing `verify_module` after each plugin.
6. **`c23-emit`:** Runs deterministic `arm64` FMA matching (section 4) and emits deterministic GNU C23 (`.c`, or `.cpp` when C++ headers are imported) using `itoa`/`ryu` fast formatting. (`-emit=c` writes this translation unit directly.)
7. **`clang-compile`:** Invokes `clang` `23.1.0` (`--target=<triple> -fuse-ld=lld`) to produce `.o`, `.s`, shared objects, or final linked binaries.

## 3. GIR trivial inlining

`gopp_gir::inline_trivial` inlines direct calls to *trivial* functions: defined in the current module, single basic block ending in `Return`, at most 12 statements, no nested `Call`, no `Assert`, no `Drop`, no `AddrOf`, and no `Local::Owned` temporary. Functions marked `@NoInline` (`InlineHint::Never`, emitted as `__attribute__((noinline))`) are never inlined; functions marked `@Inline` (`InlineHint::Always`, emitted as `__attribute__((always_inline)) inline`) bypass the 12-statement cap when the structural conditions hold. Calls with a `Cleanup` unwind edge are left in place until cleanup-edge rewiring (`P04.26`).

- **Boundary markers:** Argument (`param = arg`) and return (`dest = ret`) copies emitted by `inline_trivial` carry `Statement::Boundary`, preventing the FMA matcher (section 4) from fusing a multiply inside an inlined callee with an add/subtract in the caller.
- **Debug scopes:** Each inlined call creates a `DebugScope::Inlined` scope under the call's lexical scope, copies the callee's `DebugScope::Block` subtree underneath (`Origin` naming the callee's block), and tags copied locals with `Local::Origin` and `Local::ArgNo` (`V073`, `V074`).
- **Linear complexity:** `inline_trivial` runs in $O(|M|)$ steps, tracked by `inline_steps()` in unit stress tests.

## 4. Deterministic `arm64` FMA contraction

Go's `gc` compiler fuses floating-point `x * y` with a consuming `+` or `-` on `arm64` and never on `amd64`. Go++ matches this rule identically at every `-opt` level:

- **Single fusion point:** `gopp-codegen-c` runs `fuse_fma` on `arm64` targets only, emitting `__builtin_fma` (`double`) and `__builtin_fmaf` (`float`) where `gc`'s SSA rules fuse `FADDD a (FMULD x y) => FMADDD`, while passing `-ffp-contract=off` globally to `clang` so Clang never fuses anything else (including across `Statement::Boundary` or explicit conversions like `float64(x * y) + z`).
- **Dominator-tree + SCC reaching-product matcher (no SSA construction required):**
  - A *product* is a temporary read once or a non-address-taken local/parameter/result written by a float `*`, a negation of a product, or a non-`Boundary` copy of a product (`petgraph` `0.8.3` computes Lengauer-Tarjan dominators and topological SCC numbers).
  - Blocks are walked in dominator-tree preorder with a scoped undo log so every read sees the dominating product write regardless of physical block order.
  - Intervening writes on non-dominating CFG paths that reach the read without re-entering the product block (which would form a $\phi$ in `gc`) are detected via binary search on topological SCC numbers plus a bounded region walk (`8` steps per block/edge + `64`, linear overall).
  - Constant factors mirror `gc`'s generic rules (`x * 2.0 => x + x`, `x * 1.0 => x`, `x * -1.0 => -x`, `x / 2^k => x * 2^-k`, non-NaN constant folding).
- **Negated-FMA `-0.0` barrier on `arm64`:** On AArch64, negating `fma(a, b, c)` must produce `-0.0` when `fma(a, b, c)` evaluates to `+0.0`, whereas hardware `fnmadd` (`-(a*b) - c`) yields `+0.0`. Whenever `gopp-codegen-c` emits a negation of a fused `__builtin_fma` / `__builtin_fmaf` on `arm64`, it pins the intermediate result through an inline assembly association barrier (`__asm__("" : "+w"(_fma_tmp))`) before `-_fma_tmp`, preventing Clang's AArch64 instruction selector from folding `-(__builtin_fma(...))` into `fnmadd` at any `-O` level.

## 5. `-opt=0` with DWARF debug info

Debug info is enabled by default (`-no-debug` disables it). Because `gopp-codegen-c` emits named C23 stack variables inside nested `{ ... }` lexical blocks and compiles `-opt=0` with `clang -O0 -gdwarf-5 -fno-omit-frame-pointer`:

- Clang `-O0` naturally keeps every source local in its own stack slot without SSA promotion or dead-store elimination, so `gdb` and `lldb` inspect exact live variable values on every source line with zero `volatile` workarounds.
- Compiler temporaries (`Local::Temp`) are scoped tightly to statement blocks or expressions, keeping stack frames compact (and aggregate call scratch slots are reused per `(type, arg_index)` across sequential calls in a function).
- At `-opt=1` and above, Clang promotes C23 locals into SSA registers and emits DWARF location lists (`DW_OP_reg*` / `<optimized out>`), never stale values.

## 6. Opt-matrix verification and extension hooks

- **Optimization equivalence matrix (`tests/run`):** Builds and runs the full differential, collection, panic, FMA, and C-ABI corpora across `-opt=0`, `1`, `2`, `3`, `s`, and `z` on `amd64` and `arm64` (under `qemu-aarch64`), asserting identical stdout, stderr, and exit codes across all levels alongside DWARF verification (`dwarfdump` / `gdb` / `lldb`).
- **Profiles ([ADR-0007](../adr/0007-build-profiles.md)):** Profiles populate `PipelineRequest` (`opt`, `debuginfo`, `lto`, `panic`, `sanitize`, `strip`, `cpu`, `pgo`, `link`); build cache keys (`blake3`) hash the effective `PipelineSpec` plus loaded `-fpass-plugin` digests so equivalent profiles share cache entries.
- **`-trace`:** Emits Chrome Trace JSON spans across front-end, GIR passes, plugin invocations, C23 emission, and Clang `-ftime-trace` subprocess reports.
- **ThinLTO (`lto = "thin"`):** Passes `-flto=thin -fuse-ld=lld` to `ClangDriver` across Go++ C23 translation units and Clang-compiled `cdep` objects (`P15`).
