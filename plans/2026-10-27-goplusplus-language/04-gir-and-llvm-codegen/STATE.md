# P04 GIR and LLVM codegen: STATE

Legend: `[ ] not_started` · `[~] in_progress` · `[x] completed`
Phase status: `[~] in_progress`
Last updated: 2026-10-30 (P04.15 EH cost spike done, ADR-0028)
Task definitions: [../TODO.md](../TODO.md) (section P04) · Design: [README.md](README.md)

## Tasks

- [ ] P04.01 GIR design
- [ ] P04.02 M0 hello
- [ ] P04.03 Targets & linking
- [ ] P04.04 Lower core
- [ ] P04.05 Lower strings/slices/views/maps
- [ ] P04.06 Lower methods & interfaces
- [ ] P04.07 Lower closures, defer, panics
- [ ] P04.08 Monomorphization
- [ ] P04.09 Flow-sensitive ownership
- [ ] P04.10 `!` lowering
- [ ] P04.11 C ABI via Clang
- [ ] P04.12 DWARF
- [ ] P04.13 Codegen pipelines per `opt`
- [ ] P04.14 Differential tests vs Go
- [x] P04.15 EH cost spike: ADR-0028. On Go++-shaped IR, EH makes x86-64 FastISel codegen 1.7 to 1.8 times slower (the `invoke` and `landingpad` fallbacks to SelectionDAG); the ADR-0028 lowering rules (each `invoke` alone in its block, one state-driven pad per function, shared panic calls) bring it to 1.3 to 1.4. Kept: D4.6 and FastISel; no GlobalISel on x86-64, no LLVM patch, D6.9 not triggered. Tools in `bench/eh/`.
- [ ] P04.16 Bootstrap mode
- [ ] P04.17 Symbol names
- [ ] P04.18 Internal calling convention
- [ ] P04.19 Fast test runner
- [ ] P04.20 Borrow checker (ADR-0021)
- [ ] P04.21 Concurrency checks (ADR-0022)
- [ ] P04.22 `extends` lowering (ADR-0023)
- [ ] P04.23 Operator lowering (ADR-0024)
- [ ] P04.24 Units lowering (ADR-0025)

## Open questions

- [x] Q1 GIR serialization format details (decided: D4.15, LLVM bitstream container)
- [~] Q3 `.go` files and the borrow checker and concurrency checks: deferred by owner 2026-10-28 (Go mode is not a current focus; no new Go-mode work planned)
- [x] Q2 One LLVM module per package, or per function group: deferred to P06 (P06.07)

## Exit criteria

- [ ] M0: hello runs on linux/amd64 and linux/arm64 (QEMU)
- [ ] M0 builds on NixOS and on an FHS distro
- [ ] M1: ≥ 100 differential programs match Go's output
- [ ] GIR verifier runs in all tests; compiler is ASan-clean on the full suite
- [ ] gdb and lldb can step through samples and print locals

## Decision changes

- 2026-10-30 (ADR-0028, P04.15): D4.6 keeps native EH and FastISel; P04.07 gets four EH lowering rules for FastISel (each `invoke` alone in its block before ISel, one landing pad per function driven by a cleanup-state index, one panic call per kind with per-check source positions, no pad for callees that cannot unwind) and a fallback-count test; P04.13 chooses the arm64 `opt 0` selector with the ADR's numbers; P04.18 gets the aggregate-ABI cost at `opt 0`. Proposed, for the owner only if P06.01 triggers it: patching FastISel (an LLVM build from source).
- 2026-10-28 (ADR-0025, owner decision): new task P04.24 and design D4.20, units lowering (units erased to the base kind, scale conversions as constant `fmul` or `mul`, type descriptors with the canonical and display form for boxed unit values, `fmt` printing). Task count 261.
- 2026-10-28 (ADR-0023, owner decision): new task P04.22 and design D4.18, lowering of `extends` (type tables only for types with `@Virtual` / `@Abstract` methods, indirect calls through parent pointers, free upcast, checked downcast, `base` calls, drop order child then parent). Depends on P03.22. Final-only hierarchies have no table; owning upcasts and downcasts there are checker errors (P03.22).
- 2026-10-28 (ADR-0024, owner decision): new task P04.23 and design D4.19, operator lowering (direct calls that inline, compound forms with one evaluation, lazy and eager `&&`/`||`, `range`, `<-`, `@Convert`, user literals, fixed mangling table). Depends on P03.24.
- 2026-10-28 (owner decisions): Q3 (Go mode and the borrow and concurrency checks) is deferred, Go mode is not a current focus. P04.23 has no literal-suffix lowering any more (ADR-0024 decision 15).
- 2026-10-28 (ADR-0024, manager decisions on the open points): the conversion hook is the attribute `@Convert` on an ordinary method (not a method named `to`); `<=>` is built in for numbers and strings (like `cmp.Compare`); `@Right` is allowed on every binary operator except `&&`/`||` (non-commutative operators are written for the swapped order, e.g. `1.0 / v`); an optional in-place compound method with a pointer receiver (`func (m *Matrix) += (b Matrix)`) avoids the temporary. `select` with user channels, user `==` ignored for map keys, constraint-only operator interfaces and no `!` on operators are decided. The one open point left (literal suffixes from other packages) was closed later the same day: literal suffixes were dropped (see the units line).
- 2026-10-28 (ADR-0021 amendment, owner decisions): (A) a call result borrows what the callee's body says (per-function borrow summary, fixpoint over recursion, stored in export data with the "writes this parameter" bit; fallback to "all borrow-capable arguments" only with no body; no lifetime syntax; a changed summary is an API change for P14); (B) one writable borrow of a place at a time (Rust's `&mut` rule, spec OWN-B9): conservative overlap, writable `*T` copies are reborrows, every raw `*T` is a tracked borrow, `Shared[*T]` read-only unless a cell type (`mem.Cell` / `Mutex`). Hidden aliasing leaves UB. P04.20 extended, D4.16 updated; no new task.
- 2026-10-28 (ADR-0022, owner decision): new task P04.21 and design D4.17, race-free concurrency checks (`Send` / `Sync` labels, `go` and channel rules, globals, shared handles, deadlock check) on the flow graph of P04.09 and P04.20.
- 2026-10-28 (ADR-0021, owner decision): new task P04.20, a borrow checker without lifetime annotations on P04.09's flow graph; P04.09's "use through a pointer taken in the same function" item moves into it.
- 2026-10-27 (ADR-0007, build profiles): D4.8's three build modes become the `opt` setting of build profiles (P06 D6.12); `dev` is now `opt 0`, the default of the `debug` profile.
- 2026-10-27 (ADR-0006, P01.15 owner decisions): the `view` types are renamed `ref` (layout table only; no layout change).

## Notes / blockers

- The M0 slice (P04.01–P04.03) can start as soon as P02/P03 handle the hello subset. It doesn't need to wait for those phases to finish.
- P04.15 (EH cost spike) runs before P04.07.
- P04.14 prints through the bootstrap std (P05.15), so P05.15 must be done before M1.
- P04.19 (fast test runner) comes early, before the differential corpus grows.
