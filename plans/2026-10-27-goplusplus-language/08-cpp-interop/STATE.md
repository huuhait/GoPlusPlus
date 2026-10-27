# P08 C++ interop: STATE

Legend: `[ ] not_started` · `[~] in_progress` · `[x] completed`
Phase status: `[ ] not_started`
Last updated: 2026-10-27
Task definitions: [../TODO.md](../TODO.md) (section P08) · Design: [README.md](README.md)

## Stages

- [ ] Stage 1: free functions, POD, enums, overloads (P08.02)
- [ ] Stage 2: classes & lifetimes (P08.03)
- [ ] Stage 3: templates (P08.05)
- [ ] Stage 4: operators & iterators (P08.06)
- [ ] Stage 5: exceptions ↔ panics (P08.09)

## Tasks

- [ ] P08.01 C++ import units & header packages
- [ ] P08.02 Stage 1: free functions & POD
- [ ] P08.03 Stage 2: classes & lifetimes
- [ ] P08.04 Same-module C++ codegen
- [ ] P08.05 Stage 3: templates
- [ ] P08.06 Stage 4: operators & iterators
- [ ] P08.07 References
- [ ] P08.08 Inheritance & virtuals
- [ ] P08.09 Stage 5: exceptions ↔ panics
- [ ] P08.10 C++ stdlib builds
- [ ] P08.11 Unsupported-feature diagnostics
- [ ] P08.12 Interop samples
- [ ] P08.13 Performance parity
- [ ] P08.14 C++ ABI fuzzing
- [ ] P08.15 Decide P08 Q3–Q4

## Open questions

- [x] Q1 Escaping C++ names that are Go++ keywords: decided, trailing underscore (P07 D7.3)
- [x] Q2 `const` in Go++ signatures: moved to P07 Q2
- [ ] Q3 Explicit template arguments for function templates (task P08.15)
- [ ] Q4 Namespace flattening for deep hierarchies, and collision error wording (task P08.15)

## Exit criteria

- [ ] M5: std containers, smart pointers, a header-only template library and fmt's runtime API (package-local files) work; ASan-clean
- [ ] Exceptions ↔ panics both directions with destructors verified on both sides
- [ ] `std::vector` `operator[]` loop within 5% of clang++ -O2, accessor inlined

## Decision changes

- None.

## Notes / blockers

- fmt is built from package-local files (P07 D7.9).
