# STATE: Go++ language plan

Legend: `[ ] not_started` · `[~] in_progress` · `[x] completed`
Last updated: 2026-10-28

## Plan

- [x] Plan drafted: README, 17 phases (README + STATE each), TODO
- [x] Plan reviewed and accepted by the owner

## Phases

- [~] P00 Foundations ([state](00-foundations/STATE.md))
- [x] P01 Language specification v0 ([state](01-language-spec/STATE.md))
- [x] P02 Front-end: lexer, parser, AST, `gopp fmt` ([state](02-frontend/STATE.md))
- [~] P03 Semantic analysis ([state](03-semantic-analysis/STATE.md))
- [ ] P04 GIR and LLVM codegen ([state](04-gir-and-llvm-codegen/STATE.md))
- [ ] P05 Runtime and memory ([state](05-runtime-and-memory/STATE.md))
- [ ] P06 Build driver and compile speed ([state](06-build-driver-and-compile-speed/STATE.md))
- [ ] P07 C interop ([state](07-c-interop/STATE.md))
- [ ] P08 C++ interop ([state](08-cpp-interop/STATE.md))
- [ ] P09 Macros ([state](09-macros/STATE.md))
- [ ] P10 Modules: gopp.mod / gopp.sum ([state](10-modules-gopp-mod-sum/STATE.md))
- [ ] P11 C/C++ deps from the internet + CMake ([state](11-c-cpp-deps-and-cmake/STATE.md))
- [ ] P12 Concurrency ([state](12-concurrency/STATE.md))
- [ ] P13 Standard library v1 ([state](13-standard-library/STATE.md))
- [ ] P14 Tooling ([state](14-tooling/STATE.md))
- [ ] P15 Performance ([state](15-performance/STATE.md))
- [ ] P16 Release ([state](16-release/STATE.md))

## Milestones

- [ ] M0 Hello, LLVM: one-file `package main` → native binary via LLVM + LLD (P00, P02–P04 slice)
- [ ] M1 Go core: ≥ 100 differential tests vs `go run` pass (P02–P04)
- [ ] M2 No-GC core: ownership, destructors, `!`, panic/defer/recover; ASan/LSan clean (P05)
- [ ] M3 Fast builds: export data + cache; compile budgets enforced in CI (P06)
- [ ] M4 C bridge: C header import, package-local C, `gopp cc` (P07)
- [ ] M5 C++ bridge: std containers, templates, RAII, exceptions ↔ panics (P08)
- [ ] M6 Macros: declarative + procedural + `@Derive` (P09)
- [ ] M7 Packages: Go++ modules + internet C/C++/CMake deps locked in mod + sum (P10, P11)
- [ ] M8 Concurrency: goroutines, channels, select; TSan clean (P12)
- [ ] M9 v0.1 alpha: std v1, tooling, perf targets, release (P13–P16)

## Current focus

- P00 is done locally. What's left needs a push: the first GitHub CI run (P00.07, P00.08, P00.13 and the CI exit criteria), then CI suite-time baselines.
- P01: the spec is written, reviewed by the owner (D1–D25) and applied (ADR-0006). P01 is complete.
- Build profiles (owner request) are in the plan: P06 D6.12, P09 D9.12, P10 D10.11, P11 D11.13, ADR-0007; tasks P06.14–16, P09.13, P10.12, P11.19–20, P14.11 (240 tasks).

## Log

- 2026-10-27: Plan created. Environment observed: NixOS 26.05, 192 cores, clang 21.1.8, CMake 4.1.2, Ninja 1.13.2, Go 1.25.7, mold 2.40.4. LLVM dev libraries and LLD are not installed (handled in P00.02/P00.03).
- 2026-10-27: Pre-review fix (owner request): attached macro renamed `@derive` → `@Derive` across the plan.
- 2026-10-27: Pre-review fixes (owner decisions): every macro and attribute name is capitalized (`#Assert`, `@Extern`, `@Derive`, …); `fmt` and `encoding/json` work on any type via compiler-emitted field tables (Go-like), with `@Derive(JSON)` as an optional fast path; std derive set reduced to `JSON` and `Equal` (`Clone` removed; `clone()` stays automatic per D1.4).
- 2026-10-27: Pre-review fix (owner decision): Must moved from postfix `call()!` to `call!()`. Go's `val, err := call()` stays as-is; `!` applies to calls only (map/receive `!` forms dropped; use comma-ok).
- 2026-10-27: Pre-review fixes (owner decisions): C/C++ package headers are imported Go-style (`import "github.com/madler/zlib/zlib.h"`; system headers keep `c:`/`c++:`); cdep `target` replaced by `mod static|dynamic`; CMake only (`build` key and `sources`/`headers` recipes removed); packages without CMake use a local CMake file (`cmake ./cmake/X.cmake`); archive packages get a Go-style path plus `url`.
- 2026-10-27: Pre-review fixes (owner decisions): cdep key renamed `mod` → `mode`; system headers imported as `import "<vector>"` / `import "<stdio.h>"` (`c:`/`c++:` removed); no `C.`/`cpp.` pseudo-packages: each imported header is a package named after the file (`zlib.crc32`, `vector.vector[int32]`, `algorithm.sort`), C++ namespaces flattened; C types map to plain Go++ types; strings and slices pass to C without conversions; ABI-only helpers in std package `ffi`.
- 2026-10-27: Pre-review fix (owner decision): Go's `new` removed; allocation is explicit with `new_unique(v)` → `Unique[T]` and `new_shared(v)` → `Shared[T]`, and `new_weak(s)` makes a `Weak[T]` from a `Shared[T]`; `unique *T`/`shared *T`/`weak *T` replaced by the predeclared generic types `Unique[T]`/`Shared[T]`/`Weak[T]` (keywords `unique`/`shared`/`weak` removed); `&T{…}` is now a compile error with a fix-it.
- 2026-10-27: Pre-review fix (owner decision): `func f!(…) T` declares a function that can fail (callers see `(T, error)`); `call!()` returns the error inside functions that can fail and panics elsewhere (e.g. `main`); Go-style `val, err := call()` unchanged; errors returned through `!` carry a trace (P05.14); `#Try` macro dropped.
- 2026-10-27: Deep review started: four independent read-only reviewers (language & memory model; C/C++ interop & dependencies; compiler, build speed & phase plan; document consistency).
- 2026-10-27: Deep review finished: 89 findings from 4 reviewers, merged into 57 items (blockers: owned reads from fields, interfaces consuming values, error idioms, bootstrap order, CMake provider, transitive cdeps, CMake File API). Waiting for owner decisions before applying fixes.
- 2026-10-27: Review fixes applied to every phase.
  - Owner decisions: no macOS in v0.1 (Linux amd64/arm64 only); `s = s[i:j]` shrinks an owned slice in place, slices stay 3 words, std gets `container/deque`; errors work like Go (shared values, `err == io.EOF`); no extra memory-safety layer in v1 (known dangling cases listed as UB).
  - Coordinator calls in the owner's "like Go" direction, waiting for the owner's OK:
    1. interface values (including `error`) and func values are shared, reference-counted handles;
    2. escaping closures keep captured variables in shared cells, chosen by Go-style escape analysis (also across calls, e.g. `http.HandleFunc`);
    3. taking ownership of a value read from a field, element, map entry or other non-local place needs `clone(x)` or `move(x)`; just reading it borrows.
  - 15 new decisions: D0.12, D1.19–D1.21, D4.11–D4.13, D5.12, D6.10–D6.11, D9.11, D10.10, D11.10–D11.12. 24 new tasks (228 in total). Every phase has numbered open questions, mirrored in its STATE.md.
  - Follow-ups from the writers' reports: `gpp fmt` sorts only import groups without header imports (D2.8); seccomp blocks the network in the cdep sandbox when namespaces are unavailable (D11.3); `tidy` refreshes `// indirect` cdep blocks (D10.3); Nix workflow (`flake.lock`, `.envrc`, `nix develop -c`, D0.4) and a flake package for NixOS (D16.4).
  - Checks pass: TODO.md and every STATE.md list the same task IDs; every decision and open-question reference resolves.
- 2026-10-27: Owner approved the three coordinator calls: shared interface, `error` and func values; Go-style boxes for closures that outlive their function; `clone(x)`/`move(x)` to take ownership of a value read from a field, element or map entry.
- 2026-10-27: Consistency review of the applied fixes found 13 must-fix items, all fixed:
  - interface parameters are shared handles (`return err` works as in Go);
  - substrings and borrowed `string` parameters pass to C as temporary copies;
  - every C++ move runs the move constructor; only containers relocate;
  - goroutine switches save the C++ exception state;
  - macro bodies are part of dependents' cache keys;
  - one header-extension list; header paths are source-tree paths;
  - cdep target choice filters by `mode` first;
  - new P12 D12.12 and task P12.12: stopping goroutines at exit (229 tasks);
  - tasks moved to the phase where their dependencies exist (P04.03, P04.08, P05.12, P06.12, P07.08);
  - `/notfound` gpp.sum lines; `readonly` never on `[]T` data.
- 2026-10-27: Re-check returned CLEAN. Its 4 minor follow-ups are fixed (phase dependency links, the C++-heavy budget, non-clonable interface arguments, goroutines stopped before `main`'s locals on a panic). Plan accepted.
- 2026-10-27: Owner decisions on the P00 and P01 open questions:
  - P00 Q1: GitHub + GitHub Actions. Q2: Apache-2.0 WITH LLVM-exception. Q3: the language stays Go++, and everything else is renamed to `gopp` (CLI `gopp`, `.gopp` files, `gopp.mod`/`gopp.sum`, `.gopx`, `//gopp:` directives, `GOPP*` variables). Log entries above keep the old names.
  - P01 Q4: every owned slice and map stores its allocator, so owned slices are 4 words (new D1.22; replaces "slices stay 3 words"). Q5: generic methods allowed (D1.10). Q6: one-word interface inline storage (D1.12).
  - P01.12 is done, so P01 is in progress.
- 2026-10-27: Fast dev loop for `gopp` itself (owner request): README principle 7; P00 D0.13 with budgets (edit → rebuilt ≤ 10 s, stage tests ≤ 10 s, full check ≤ 5 min, one test ≤ 2 s, clean build ≤ 2 min) and tasks P00.13–P00.14; P04 D4.14 fast test runner (batch compiles, ORC JIT in executor processes, Go outputs saved as golden files) and task P04.19; agent rules added to the repo's `CLAUDE.md`. 232 tasks.
- 2026-10-27: P00 built through the team workflow (3 coders in parallel, PM merge on c3's LLVM-style base, review loop until a fresh reviewer returned CLEAN; ledger `.claude/team/p00-foundations-ledger.md`, 5 findings, all closed).
  - Done locally: P00.01–P00.06, P00.09–P00.12, P00.14, plus the D0.13 budgets (edit → rebuild 0.2–1.7 s, clean build ~9–14 s, full `ninja check` < 1 s).
  - Waiting on the first GitHub CI run (needs a push): P00.07, P00.08, P00.13.
  - Decision changes (ADR-0002): sccache replaces ccache (owner request; automatic, 100 GB); LLVM 23.1.0 pinned; Release links LLVM, Clang and LLD fully statically.
- 2026-10-27: P01 spec written through the team workflow (1 coder, no merge, review loop until a fresh reviewer returned CLEAN; ledger `.claude/team/p01-language-spec-ledger.md`, 58 findings over 20 rounds, all closed).
  - Done: P01.01–P01.11, P01.13, P01.14, P01.16, and every exit criterion. `docs/spec/` has 16 chapters and 216 numbered rules; `docs/spec/examples/` has 10 samples whose Go versions match their expected output.
  - Left: P01.15, the owner review of the 47 `Open (P01 review)` notes and the ADR-0003/0004 update.
- 2026-10-27: P01.15 started. The owner made 8 decisions (D1–D8 in `01-language-spec/STATE.md`, "P01.15 owner decisions"). Main ones: `own ...T` for owning variadics; map insert during `range` always panics; `x.Clone()` with `clone(x)` as a short form; keyword `view` → `ref` (`@View` → `@Ref`); `case string:` also matches `ref string`; new `for _, &d := range xs` form that writes into the collection. Recorded only; they are applied to the spec, plan and ADRs when the whole review is done.
- 2026-10-27: P01.15 applied through the team workflow (1 coder, review loop until a fresh reviewer returned CLEAN; ledger `.claude/team/p01-15-apply-ledger.md`, 46 findings, all closed). D1–D25 are in `docs/spec/` (227 rules, no Open notes left), the plan READMEs/TODO and ADR-0003/0004/0006. Main changes: `ref` (was `view`), `move` parameters (was `own`), `x.Clone()` with `[T Clone]` / `[T Copy]`, `for _, &d := range`, `defer` in loops like Go (lifetime extension), C/C++ flags in `gopp.mod`, `@Name(args)` field attributes, `#M(..., func(){…})` blocks.
- 2026-10-27: Build profiles added to the plan through the team workflow (1 coder, review until a fresh reviewer returned CLEAN; ledger `.claude/team/build-profiles-ledger.md`, 10 findings, all closed). `profile` blocks in `gopp.mod` (built-in `debug`/`release`, `inherits`, `deps`), CLI `-release`/`-profile=`, the profile passed to CMake (`GOPP_PROFILE`, build type, flags; `link` is a preference unless a cdep sets `mode`), `#Profile()`/`#Setting()`/`@If`, `//gopp:build debug`, and `./target/<profile>/`. ADR-0007. 240 tasks.
- 2026-10-27: P02.01–P02.06 (source manager, diagnostics with a diag-ID table, lexer, AST, parser core, macros & attributes) built through the team workflow (3 coders, PM merge, review until CLEAN; 2 findings, both fixed). `gopp parse --dump-ast/--dump-tokens` exists. P02 is in progress; P02.07–P02.11 remain.
- 2026-10-27: D9 follow-up decided by the owner (option b): on a raw `*T` whose method set already has an `IsNil`, `p.IsNil()` is a compile error (fix-its `(*p).IsNil()` / `p == nil`); `Unique`/`Shared`/`Weak` keep their own `IsNil`. Applied to OWN-P5/P9, P01 D1.7 and ADR-0006 D9 (1 coder, fresh review CLEAN). P01.15 is done, so **P01 is complete**.
- 2026-10-27: Owner decisions on the P03 open questions: Q1 numbered error codes (`error[E0042]`, `gopp explain`, internal names kept, strict stability promise from v0.1; D3.11); Q2 `vet` built into the checker, run by `gopp vet`, the LSP, and a small subset by `gopp test` (D3.12).
- 2026-10-27: New task P03.14 (error codes & `gopp explain`, D3.11). 241 tasks.
- 2026-10-27: P02.07–P02.11 (error recovery with 31 tailored messages and fix-its, `gopp parse -j/-fixit`, `gopp fmt` matching gofmt byte-for-byte on 7284/7354 GOROOT files, 28.7 CPU-hours of fuzzing with no crash, 4.57 M lines/s per core) built through the team workflow (3 coders, PM merge, review until a fresh reviewer returned CLEAN; ledger `.claude/team/p02-frontend-tools-ledger.md`, 15 findings, all fixed). **P02 is complete.**
- 2026-10-27: Owner decision on P04 Q1 (GIR serialization format): LLVM bitstream container (as Clang PCH and Swift modules), readable only by the same compiler version, deterministic bytes for the cache, `gopp gir dump` for debugging and round-trip tests (P04 D4.15; noted in P06 D6.2).
- 2026-10-27: Owner decision on P05 Q1 (arenas with destructors): support both. `mem.Arena` stays `Copy`-only and instant to free; new `mem.DropArena` accepts any type, records drops only for types that need them, and runs them newest first on `Free`/`Reset` (P05 D5.3). New task P05.17. 242 tasks.
- 2026-10-27: Owner decision on P06 Q3: link with LLD as a subprocess (`gopp` starts its own bundled LLD as `gopp lld …`), for crash safety, no leftover LLD state in long-running processes, and parallel links in `gopp test`; the ~1–3 ms start cost per link is accepted (P06 Q3, P04 D4.9).
- 2026-10-27: Owner decisions on P07 Q1–Q3 (ADR-0008; P07.17 done): C function pointer types are `ffi.Func[func(…) R]` (no new syntax); `const` stays metadata, enforced at the C/C++ boundary against read-only bindings (no `*const T`; also P08 Q2); type-defining macros (`#define z_off_t long`) import as type aliases.
- 2026-10-27: Owner decisions on P08 Q3–Q4 (ADR-0010; P08.15 done): explicit template arguments use Go's generic brackets (`memory.make_unique[Foo](1, 2)`; first arguments given, the rest deduced; `bool` and enum constants allowed; call-only in v0); deep namespaces use the short form while it is unique (`asio.tcp`), and the full path (`asio.ip.tcp`) always works; collisions are errors on use, listing the candidates.
- 2026-10-27: P03.01–P03.07 done (team build, 25 review findings fixed): package loader, exact constants (ADR-0009, no libbf), scopes, types core, statements, generics (154/167 types2 generics files, 92.2%) and type-level ownership (63 rule-table tests). Overall types2 conformance 262/305 (85.9%), below the phase exit's 90%. Manager decisions in spec EXP-2: parts of an owned temporary are temporaries; conversions that share storage are checked as their operand. P03 in progress (P03.08–P03.14 left).
- 2026-10-27: Owner decisions on P09 Q2–Q4 (ADR-0011; P09.11 done): procedural macros build syntax with one constructor per node (no format strings, no quotes in v0; `#Quote` maybe in v1); no macros where a type goes in v0; new D9.13 `#Run(f(args))` runs one call from another package while compiling (constants, or static data for a package-level `var`; nothing runs automatically). New task P09.14. 243 tasks.
- 2026-10-27: Owner decision on P10 Q2: Go++ does not consume Go modules in v1 (importing one is an error that says so); revisit after v1.0, since it needs a dual engine (Go semantics with GC next to Go++). Added to the README non-goals.
- 2026-10-27: Owner decision on P11 Q1: CMake is the only C/C++ build system in v1 (a package without CMake gets a small local CMake file); Meson, Autotools and others are reconsidered after v1.
- 2026-10-27: Owner decision on P11 Q2: no binary caches for cdeps, signed or not, in v1 or later; every cdep is built from source whose hash is in `gopp.sum` (local and CI caches cover rebuilds). Added to the README non-goals.
- 2026-10-27: Owner decision on P11 Q3: no Windows sandbox. cdep builds on Windows run unsandboxed, like Linux without Landlock (warning; `-hermetic` refuses); checking dependency build scripts is the user's job. `find_package`/`FetchContent` stay locked through the dependency provider.
- 2026-10-27: Owner decision on P11 Q4 (ADR-0012): host builds of generator tools use a third built-in profile, `host` (`opt 2`, `debuginfo none`, no sanitizers/LTO/PGO, host baseline CPU), never the main profile's settings; `debug` and `release` share one cached tool build; `profile host ( … )` changes it (P06 D6.12, P10 D10.11, P11 D11.12).
- 2026-10-27: Owner decision on P12 Q1 (ADR-0013; P12.02 done): goroutine stack overflows are detected by prologue checks in every Go++ function (shared with the preemption check) plus guard pages while the mapping budget allows (half of `vm.max_map_count`); calls into C need 64 KiB headroom (provisional); default stack reservation 1 MiB on 64-bit, lazily committed.
- 2026-10-27: Owner decision on P12 Q2: no io_uring in v1; file I/O uses thread handoff like Go on every OS. io_uring may come after v1 as an optional Linux-only speedup, with the thread path as the fallback.
- 2026-10-27: Owner decision on P12 Q3: no async (signal) preemption; the prologue and loop back-edge checks (D12.3) are the only preemption mechanism, placed smartly (none in short known loops, once every 1024 iterations in tight loops), because a signal could stop a goroutine inside C/C++ code (e.g. holding malloc's lock) and deadlock the thread.
- 2026-10-27: Owner decision on P13 Q2 (ADR-0014): `sync.Pool[T]` stays, ownership-aware (`Get` returns an owned value, `Put(move(x))`); a runtime goroutine drops objects not taken for a few seconds (like Go's GC clearing pools); an optional `Max` caps how many it keeps.
- 2026-10-27: Owner decision on P13 Q3 (ADR-0015; P13.13 done): time zone data comes from the OS first (`$ZONEINFO`, `/usr/share/zoneinfo`), with an embedded tzdata copy as an automatic fallback, linked only into programs that use `LoadLocation`; `-tags notzdata` opts out (P13 D13.8).
- 2026-10-27: P03.08–P03.14 done (team build, 8 review findings fixed): `!` typing, `ForeignScope` with a mock, parallel body checking (deterministic, about 2x at 8 threads; 935k lines/s/core on one thread), a type serialization interface, `gopp check` and `gopp vet` v0, and numbered error codes with `gopp explain` and a lock check. All P03 tasks are done; P03 stays in progress on two exit criteria: the P01 samples need std packages (P13), and overall types2 conformance is 85.9% (generics 92.2%).
- 2026-10-27: Owner decision: required struct fields (ADR-0016). `ID int @Required` (built-in field attribute) must be set in every literal; strict: such a struct has no zero value (joins P01 rule 9 / OWN-R9; spreads to containing types), so `var u User` and `make([]User, n)` are errors while `make([]User, 0, n)` is fine. New P01 rule 11 and open question P01 Q9 (map reads of types with no zero value); new task P03.15 (P03 has one task left again); JSON decoding rejects a missing required field (P09.08, P13.08). 244 tasks.
- 2026-10-27: Owner decisions after review of ADR-0016: JSON decoding is `json.Decode[T]` / `dec.Decode[T]()` (a fresh value); `json.Unmarshal` and the other fill-a-pointer decoders are removed. P01 Q9 decided: for types with no zero value `v := m[k]` is an error, and the comma-ok forms (`x.(T)`, `clone(m[k])`, `move(m[k])`) work with the value counted as moved where `ok` is false (checked by P04.09's use-after-move). Review fixes: rule 5/OWN-R5, CON-6, GEN-5, D1.14 and literal gaps. `clear(s)` on a slice of such a type is an error (use `s = s[:0]`); `clear(m)` stays fine.
- 2026-10-27: Owner decision (ADR-0016): `@Required` on a nilable field (`*T`, `Unique`, `Shared`, interface, func, map, chan) means never nil, checked at compile time (stored values must be known non-nil, e.g. inside `if p != nil`; `move` out is an error, use the new `mem.Replace`; not allowed on `Weak`; on slices it only means "must be set"). `*T` stays as it is (non-owning pointers).
- 2026-10-27: Second review of ADR-0016 fixed: `move` out of a non-local place of a Go++ type with no zero value is an error (use `mem.Replace`, `delete`, `range move(xs)`); `&` of a never-nil field only inside `mem.Replace`; `drop` never sees such a field nil; `v, ok := <-ch` and plain `v, ok := m[k]` join the comma-ok rule; never-nil doesn't apply to type-parameter fields.
- 2026-10-27: Third review of ADR-0016 fixed: "local" defined for the move-out rule (declared variables, bindings, owning range variables, `move` parameters; not captured variables in a closure); swaps allowed; a map range variable counts as moved after `move(m[k])` of its entry; `sync.Pool[T]` needs `New` for a `T` with no zero value.
- 2026-10-27: Fourth review of ADR-0016 fixed: type-assertion and type-switch bindings are borrowed (OWN-R3b), not locals for the move rule (use `u.Clone()`); swaps evaluate all operands and checks before moving; `mem.Replace[T any](p *T, v T) T` signature in P05 D5.3.
- 2026-10-27: Fifth review of ADR-0016 fixed: for a value type with no zero value, `m[k]` and `<-ch` may be used only in comma-ok forms (incl. `case v, ok := <-ch:` in `select`), as a write target or in `delete`; `m[k].f`, `f(m[k])`, `case v := <-ch:` and the like are errors.
- 2026-10-27: Sixth review of ADR-0016 fixed: for a value type with no zero value, `clone(m[k])` and `move(m[k])` produce a value only in the comma-ok form; a bare `move(m[k])` statement only removes the entry.
- 2026-10-27: Follow-ups to ADR-0016 in other phases: P03.15 also adds error codes (P03.14) and serializes `@Required` / "no zero value" (P03.11) so they hold across packages; P09.04's macro API exposes `field.Required()`; P14.06 gets an "add missing required fields" code action; UB-9 lists C writing nil into a never-nil field.
- 2026-10-28: Owner decisions from the pointer review (ADR-0017): `Weak[T]` has no auto-deref (read it with `w.Lock()`; OWN-P5, new task P03.16, 245 tasks); spec chapter 4 starts with an "at a glance" table of the pointer kinds and borrows. `IsNil()` and the `new_weak` name stay as they are (owner decision).
- 2026-10-28: Owner decision (ADR-0018): pointers are never nil, for maximum safety over Go source compatibility. New spelling `Unique[*T]`, `Shared[*T]`, `Weak[*T]`; the pointer kinds have no zero value; "maybe" is the new predeclared `Option[T]` for any type (zero value `nil`, pointer-sized for pointer kinds), checked with `o != nil`, `!o.IsNil()` or `v, ok := o.Get()` (flow typing in the checker); `IsNil` only on `Option`; `Weak.Lock()` returns `Option[Shared[*T]]`; C `T*` imports as `Option[*T]` (`*T` when non-null), C++ `T&` is `*T`; `@Required` on pointer fields is an error. Spec chapters 3, 4, 6, 10, 16, the binarytrees example, P01, P03, P04, P05, P07, P08, P13 and the root README updated. New task P03.17. 246 tasks.
- 2026-10-28: Owner decisions, ADR-0018 amendment: (1) `Option[T]` is comparable when `T` is (both nil, or both set and equal; `Option[*T]` by address; map keys; `o == 5` means `o == Option[int](5)`); (2) a copy of a checked `Option` takes the value type; (3) funcs and interfaces with methods are never nil like pointers, `Option[Logger]` / `Option[func()]` are the "maybe" forms; `error` (and its aliases), the empty interface `any`, `unsafe.Pointer`, maps, slices and channels keep Go's `nil`; `recover()` and `panic(nil)` as in Go; `@Required` is an error on pointer, func and method-interface fields; (4) flow typing in the checker also covers `v, err := f()` and the comma-ok forms when `v` has no zero value (`n, err := r.Read(buf)` keeps `n`). Manager decisions in the same review: an absent allocator is `Option[mem.Allocator]`; use after move through a pointer taken in the same function is a compile error (P04.09), from elsewhere UB-8; a `:=` redeclaration of a read-only binding is an error. ADR-0016/0017 marked "parts changed by ADR-0018". Spec chapters 3, 4, 6, 8, 9, 10, 11, 12 and 16, P01, P03, P04, P05 and the root README updated. New task P03.18 (code for points 3-4); Option equality, hashing and layout added to P04.04/P04.06/P04.07/P05.06, `panic(nil)` to P05.10. 247 tasks.
