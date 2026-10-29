# P14 Tooling: STATE

Legend: `[ ] not_started` · `[~] in_progress` · `[x] completed`
Phase status: `[ ] not_started`
Last updated: 2026-10-28 (units LSP note)
Task definitions: [../TODO.md](../TODO.md) (section P14) · Design: [README.md](README.md)

## Tasks

- [ ] P14.01 CLI consolidation
- [ ] P14.02 `gopp test`
- [ ] P14.03 Bench & fuzz
- [ ] P14.04 Coverage
- [ ] P14.05 LSP core
- [ ] P14.06 LSP advanced
- [ ] P14.07 Debugger support
- [ ] P14.08 Editors
- [ ] P14.09 `gopp doc`
- [ ] P14.10 Distribution & `toolchain`
- [ ] P14.11 `gopp new`

## Open questions

- [ ] Q1 `gopp tool`: how third-party tools are versioned and run

## Exit criteria

- [ ] VS Code works as a daily driver on samples and std (diagnostics, completion, go-to-definition, formatting, debugging)
- [ ] LLDB and GDB display owned strings, slices and maps correctly
- [ ] Coverage and fuzzing work; fuzzing finds a seeded bug
- [ ] LSP completion p95 < 100 ms on a 100k-line workspace

## Decision changes

- 2026-10-28 (ADR-0025, owner decision): the LSP supports units: hover with the unit and canonical form, go-to-definition, completion, references and rename of unit names in `[ ]`, and quick fixes for unit mismatches (P14.06). No new task.
- 2026-10-28 (ADR-0024, owner decision): the LSP supports operator methods: hover and go-to-definition on operator expressions, conversions, indexes, and `range` loops; find references; "add operator method" action; quick fixes for blocked operators (P14.06 note; depends on P03.24 and P04.23).
- 2026-10-28 (ADR-0023, owner decision): the LSP supports `extends`: go to override / find overrides, `base.M` definition, "implement abstract methods", rename of virtual methods, and a quick fix for struct embedding (P14.06 note; depends on P03.22 and P04.22).
- 2026-10-27 (ADR-0006, P01.15 owner decisions): `view` is renamed `ref` in hover text and pretty printers.
- 2026-10-27 (ADR-0007, build profiles): D14.1 gets `gopp new`, which writes `target/` into the new project's `.gitignore`.

## Notes / blockers

- A minimal `gopp lsp` (diagnostics from `gopp check` + formatting) is worth shipping right after P03. Dogfooding the editor experience early catches diagnostic-quality issues.
- The early LSP uses the P04 source mode (P04 D4.11).
