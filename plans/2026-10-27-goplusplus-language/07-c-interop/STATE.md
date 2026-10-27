# P07 C interop: STATE

Legend: `[ ] not_started` · `[~] in_progress` · `[x] completed`
Phase status: `[ ] not_started`
Last updated: 2026-10-27
Task definitions: [../TODO.md](../TODO.md) (section P07) · Design: [README.md](README.md)

## Tasks

- [ ] P07.01 Clang workers
- [ ] P07.02 Lazy translation & header packages
- [ ] P07.03 Type mapping
- [ ] P07.04 Calls
- [ ] P07.05 C macros
- [ ] P07.06 Header inline functions
- [ ] P07.07 Strings & memory helpers
- [ ] P07.08 Callbacks
- [ ] P07.09 Package-local C/C++/asm and own-module headers
- [ ] P07.10 `gopp cc` / `c++` / `ar`
- [ ] P07.11 Import unit caching
- [ ] P07.12 ABI fuzzing
- [ ] P07.13 Export to C
- [ ] P07.14 musl cross builds
- [ ] P07.15 Header torture tests
- [ ] P07.16 Struct tags & transparent unions
- [x] P07.17 Decide P07 Q1–Q3 (owner decisions, ADR-0008)

## Open questions

- [x] Q1 Spelling of C function pointer types (decided: `ffi.Func[F]`, ADR-0008)
- [x] Q2 Constness in the Go++ type system, including C++ methods and parameters (decided: metadata, enforced at the boundary, ADR-0008)
- [x] Q3 Translating type-defining macros (decided: yes, as type aliases, ADR-0008)

## Exit criteria

- [ ] M4: samples (zlib, sqlite amalgamation, qsort callback, printf) run on linux/amd64 and linux/arm64
- [ ] ABI fuzzing: ≥ 10k signatures per target, 0 mismatches
- [ ] Standard header set + zlib + sqlite3 import; warm import overhead ≤ 20 ms per package (provisional)

## Decision changes

- 2026-10-27 (ADR-0006, P01.15 owner decisions): `//gopp:cflags` and `//gopp:ldflags` are removed; the module-wide `cflags` and `ldflags` directives of gopp.mod replace them. `//gopp:build` stays (P01 D1.15).

## Notes / blockers

- P07.01–P07.04 can start right after M0, because they only need the codegen ABI path (P04.11). They do not need all of P05.
- The minimal embedded Clang for ABI classification is built early, in P04's bootstrap mode (P04 D4.11). P07.01 builds on it.
