# P04 GIR and LLVM codegen: STATE

Legend: `[ ] not_started` · `[~] in_progress` · `[x] completed`
Phase status: `[ ] not_started`
Last updated: 2026-10-28 (P04.21 concurrency checks added)
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
- [ ] P04.15 EH cost spike
- [ ] P04.16 Bootstrap mode
- [ ] P04.17 Symbol names
- [ ] P04.18 Internal calling convention
- [ ] P04.19 Fast test runner
- [ ] P04.20 Borrow checker (ADR-0021)
- [ ] P04.21 Concurrency checks (ADR-0022)

## Open questions

- [x] Q1 GIR serialization format details (decided: D4.15, LLVM bitstream container)
- [ ] Q3 `.go` files and the borrow checker and concurrency checks (decide before P04.20 and P04.21)
- [x] Q2 One LLVM module per package, or per function group: deferred to P06 (P06.07)

## Exit criteria

- [ ] M0: hello runs on linux/amd64 and linux/arm64 (QEMU)
- [ ] M0 builds on NixOS and on an FHS distro
- [ ] M1: ≥ 100 differential programs match Go's output
- [ ] GIR verifier runs in all tests; compiler is ASan-clean on the full suite
- [ ] gdb and lldb can step through samples and print locals

## Decision changes

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
