# ADR-0037: Version 1 targets `linux/amd64` and `windows/amd64`

- **Status:** accepted (owner decision)
- **Date:** 2026-11-11
- **Plan reference:** D16.1, P04.29, P04.33, P16; replaces the eight supported targets of [ADR-0001](0001-implementation-language.md); related [targets-and-linking.md](../design/targets-and-linking.md), [ADR-0002](0002-clang-version-policy.md)

## Context

The target table of the design has eight targets: Linux on amd64, arm64 and riscv64, macOS on arm64 and amd64, Windows on amd64 and arm64, and `wasip1/wasm32`. Nothing said which of them a release must work on. Promising all eight would make every change wait for platforms nobody can test today; promising none would guarantee nothing.

## Decision

1. **Version 1 supports two targets: `linux/amd64` and `windows/amd64`.** Both must pass every test before a change is done and before a release.
2. **The other six targets are decided after version 1.** Until then they are not tested, not promised, and no work is planned for them. `TargetSpec` keeps their rows so that nothing in the compiler assumes one platform, but a build for one of them reports that the target is not supported in this version.
3. **Tests that the design documents place on `linux/arm64`** (under `qemu-aarch64`) are deferred with that target. Where a test compares two targets, the two are `linux/amd64` and `windows/amd64`.
4. Windows tests run on a real Windows machine. Its address is local configuration and is not written in this repository.

## Consequences

- Every piece that touches the operating system needs a Windows form from the start, not as a later port: linking against the Microsoft C runtime with `lld-link`, PE/COFF objects, panics and their unwinding, goroutine stacks, file and process APIs of the library, and path handling in the tools.
- The fast test runner ([test-runner.md](../design/test-runner.md)) is written for Linux (`dlopen`, resource limits, `/proc`). On Windows the run tests use real executables until the runner has a Windows form.
- C `long` is 4 bytes on Windows, so the rules for imported C types are exercised on both data models from the start.
- The 32-bit questions of `wasm32`, and macOS, arm64 and riscv64 specifics, leave the path to version 1.
- The dev shell still carries `qemu-user` and an arm64 sysroot; they stay unused until arm64 is decided.

## Alternatives

- **Three tiers over all eight targets** (the plan's first proposal): spreads the promise thin and needs machines that do not exist here (a Mac) or work that is a project of its own (goroutines on wasm).
- **Linux only:** the least work, but Windows is where the differences are (object format, linker, C runtime, unwinding, data model); finding them after version 1 would mean redesign.
